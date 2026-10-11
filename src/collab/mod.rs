//! Drawing together: kids on the same network share one picture in real time,
//! with one kid's app coordinating (no server, no internet).
//!
//! Every app keeps three pictures in step:
//! - `confirmed`: every edit the hub has put in order, applied in that order;
//! - `base`: `confirmed` plus my own edits the hub hasn't confirmed yet;
//! - the picture on screen: `base` plus whatever I'm in the middle of.
//!
//! When someone else's edit arrives, it goes under my unconfirmed edits and under
//! whatever I'm drawing right now (the app re-renders that on top), which is
//! exactly the order the hub gave everyone, so every screen ends up identical.

pub mod discovery;
pub mod hub;
pub mod names;
pub mod net;
pub mod patch;
pub mod protocol;

use std::collections::{BTreeMap, HashMap, VecDeque};

use image::RgbaImage;

use crate::raster::{self, IRect};
use patch::{Edit, Patch, Region};
use protocol::{FromHub, Kid, PeerId, Presence, TILE, Tile, ToHub};

/// What a message from the hub changed, for the app to follow up on.
#[derive(Debug, PartialEq)]
pub enum Received {
    Nothing,
    /// `base` changed here: rebuild the screen from it.
    Picture(Region),
    Kids,
}

pub struct Session {
    pub me: PeerId,
    /// The kid whose app runs the room.
    pub host: PeerId,
    pub kids: Vec<Kid>,
    /// Whether newcomers wait for the host to let them in.
    pub ask_first: bool,
    /// When hosting: kids waiting to be let in.
    pub waiting: Vec<Kid>,
    confirmed: RgbaImage,
    seq: u64,
    /// `confirmed` plus my unconfirmed edits.
    pub base: RgbaImage,
    pending: VecDeque<(u64, Edit)>,
    next_id: u64,
    outbox: Vec<ToHub>,
    /// How many edits from other kids have arrived.
    pub foreign: u64,
    /// Changes with every edit to the picture, from anyone (for "unsaved changes").
    pub version: u64,
    /// Friends' drawings in progress, tile by tile.
    previews: BTreeMap<PeerId, HashMap<(u16, u16), Patch>>,
    /// All of those drawn together, shown over the picture (transparent elsewhere).
    pub overlay: RgbaImage,
    /// Where `overlay` changed since the app last looked.
    pub overlay_dirty: IRect,
    /// My drawing in progress as friends last saw it, tile by tile.
    sent: HashMap<(u16, u16), Patch>,
    /// Compare every tile next time (my preview was cleared everywhere).
    resend_all: bool,
    last_presence: Option<Presence>,
    /// Friends' pointers, with when each last changed.
    pub pointers: BTreeMap<PeerId, (Presence, f64)>,
    /// The app's clock, for timing pointers.
    pub now: f64,
}

impl Session {
    pub fn new(me: PeerId, host: PeerId, seq: u64, picture: RgbaImage, kids: Vec<Kid>) -> Self {
        Self {
            me,
            host,
            kids,
            ask_first: false,
            waiting: Vec::new(),
            base: picture.clone(),
            confirmed: picture,
            seq,
            pending: VecDeque::new(),
            next_id: 1,
            outbox: Vec::new(),
            foreign: 0,
            version: 0,
            previews: BTreeMap::new(),
            overlay: RgbaImage::new(1, 1),
            overlay_dirty: IRect::EMPTY,
            sent: HashMap::new(),
            resend_all: false,
            last_presence: None,
            pointers: BTreeMap::new(),
            now: 0.0,
        }
        .with_blank_overlay()
    }

    fn with_blank_overlay(mut self) -> Self {
        self.clear_previews();
        self
    }

    /// Forget every friend's preview (the picture changed size, say).
    fn clear_previews(&mut self) {
        let (w, h) = self.base.dimensions();
        self.previews.clear();
        self.overlay = RgbaImage::new(w, h);
        self.overlay_dirty = IRect::of_image(&self.overlay);
    }

    /// The room moved to a new host: start again from its picture, with my
    /// unconfirmed edits on top and sent again (my undo history stays valid).
    pub fn rejoin(&mut self, me: PeerId, host: PeerId, seq: u64, picture: RgbaImage, kids: Vec<Kid>) {
        (self.me, self.host, self.seq, self.kids) = (me, host, seq, kids);
        self.waiting.clear();
        self.confirmed = picture;
        self.base = self.confirmed.clone();
        for (_, edit) in &self.pending {
            edit.apply(&mut self.base);
        }
        self.outbox = self.pending.iter().map(|(id, edit)| ToHub::Edit { id: *id, edit: edit.clone() }).collect();
        self.sent.clear();
        self.resend_all = true;
        self.last_presence = None;
        self.pointers.clear();
        self.clear_previews();
        self.version += 1;
    }

    /// Whether any friend is in the middle of drawing something.
    pub fn has_previews(&self) -> bool {
        self.previews.values().any(|tiles| !tiles.is_empty())
    }

    /// Redraw one tile of the overlay from everyone's previews.
    fn recompose(&mut self, (tx, ty): (u16, u16)) {
        let r = Tile::rect(tx, ty).intersect(IRect::of_image(&self.overlay));
        if r.is_empty() {
            return;
        }
        raster::fill_rect(&mut self.overlay, r, [0, 0, 0, 0]);
        for tiles in self.previews.values() {
            if let Some(p) = tiles.get(&(tx, ty)) {
                p.apply_within(&mut self.overlay, r);
            }
        }
        self.overlay_dirty = self.overlay_dirty.union(r);
    }

    /// A friend finished (or left): their preview goes away.
    fn drop_preview(&mut self, kid: PeerId) {
        if let Some(tiles) = self.previews.remove(&kid) {
            for k in tiles.into_keys() {
                self.recompose(k);
            }
        }
    }

    /// The tiles of what I'm drawing right now (`screen` minus the shared
    /// picture) that changed since friends last saw them, within `changed`.
    pub fn preview_tiles(&mut self, screen: &RgbaImage, changed: IRect) -> Vec<Tile> {
        if screen.dimensions() != self.base.dimensions() {
            return Vec::new();
        }
        let bounds = IRect::of_image(screen);
        let area = if std::mem::take(&mut self.resend_all) { bounds } else { changed.intersect(bounds) };
        if area.is_empty() {
            return Vec::new();
        }
        let mut out = Vec::new();
        for ty in area.y0 / TILE..=(area.y1 - 1) / TILE {
            for tx in area.x0 / TILE..=(area.x1 - 1) / TILE {
                let key = (tx as u16, ty as u16);
                let now = Patch::diff(&self.base, screen, Tile::rect(key.0, key.1));
                if now.as_ref() == self.sent.get(&key) {
                    continue;
                }
                match &now {
                    Some(p) => self.sent.insert(key, p.clone()),
                    None => self.sent.remove(&key),
                };
                out.push(Tile { tx: key.0, ty: key.1, patch: now });
            }
        }
        out
    }

    /// Where my pointer is, if that changed.
    pub fn presence(&mut self, p: Presence) -> Option<ToHub> {
        (self.last_presence != Some(p)).then(|| {
            self.last_presence = Some(p);
            ToHub::Presence(p)
        })
    }

    /// The picture the hub has confirmed (everything except my unconfirmed edits).
    pub fn confirmed(&self) -> &RgbaImage {
        &self.confirmed
    }

    /// My edits still on their way to the hub and back.
    #[cfg(test)]
    pub fn unconfirmed(&self) -> usize {
        self.pending.len()
    }

    /// A finished edit of mine (already on screen): send it, and count it as
    /// part of `base` until the hub confirms it.
    pub fn commit(&mut self, edit: Edit) {
        edit.apply(&mut self.base);
        let id = self.next_id;
        self.next_id += 1;
        self.outbox.push(ToHub::Edit { id, edit: edit.clone() });
        self.pending.push_back((id, edit));
        self.version += 1;
        // Friends drop my preview when this edit reaches them.
        self.sent.clear();
        self.resend_all = true;
        if self.overlay.dimensions() != self.base.dimensions() {
            self.clear_previews();
        }
    }

    pub fn take_outbox(&mut self) -> Vec<ToHub> {
        std::mem::take(&mut self.outbox)
    }

    /// Queue a message for the hub, in order with my edits.
    pub fn send(&mut self, msg: ToHub) {
        self.outbox.push(msg);
    }

    /// Whether this message replaces the whole picture under someone else's
    /// hands; the app first finishes whatever it's doing.
    pub fn replaces_picture(&self, msg: &FromHub) -> bool {
        matches!(msg, FromHub::Sequenced { from, edit: Edit::Replace(_), .. } if *from != self.me)
    }

    pub fn receive(&mut self, msg: FromHub) -> Received {
        match msg {
            FromHub::Welcome { .. } | FromHub::Asking(_) | FromHub::Rejected(_) | FromHub::Ping => Received::Nothing,
            FromHub::Kids(kids) | FromHub::KickedOut { kids, .. } => {
                self.kids = kids;
                let gone: Vec<PeerId> = self.previews.keys().filter(|id| !self.kids.iter().any(|k| k.id == **id)).copied().collect();
                for id in gone {
                    self.drop_preview(id);
                }
                self.pointers.retain(|id, _| self.kids.iter().any(|k| k.id == *id));
                Received::Kids
            }
            FromHub::Waiting(kids) => {
                self.waiting = kids;
                Received::Nothing
            }
            FromHub::AskFirst(on) => {
                self.ask_first = on;
                Received::Nothing
            }
            FromHub::Preview { from, tiles } => {
                let theirs = self.previews.entry(from).or_default();
                let keys: Vec<(u16, u16)> = tiles.iter().map(|t| (t.tx, t.ty)).collect();
                for t in tiles {
                    match t.patch {
                        Some(p) => theirs.insert((t.tx, t.ty), p),
                        None => theirs.remove(&(t.tx, t.ty)),
                    };
                }
                for k in keys {
                    self.recompose(k);
                }
                Received::Nothing
            }
            FromHub::Presence { from, presence } => {
                self.pointers.insert(from, (presence, self.now));
                Received::Nothing
            }
            FromHub::Sequenced { seq, from, id, edit } => {
                self.seq = seq;
                let mine = from == self.me && self.pending.front().is_some_and(|(p, _)| *p == id);
                let region = edit.region(&self.confirmed);
                edit.apply(&mut self.confirmed);
                if mine {
                    // Already part of `base` and the screen.
                    self.pending.pop_front();
                    return Received::Nothing;
                }
                self.foreign += 1;
                self.version += 1;
                self.drop_preview(from);
                let region = self.rebuild(region);
                if region == Region::Whole && self.overlay.dimensions() != self.base.dimensions() {
                    self.clear_previews();
                    self.sent.clear();
                    self.resend_all = true;
                }
                Received::Picture(region)
            }
        }
    }

    /// Recompute `base` where `confirmed` changed, keeping my unconfirmed edits on top.
    fn rebuild(&mut self, region: Region) -> Region {
        let same_size = self.base.dimensions() == self.confirmed.dimensions();
        let only_patches = self.pending.iter().all(|(_, e)| matches!(e, Edit::Patch(_)));
        match region {
            Region::Rect(r) if same_size && only_patches => {
                raster::copy_rect(&mut self.base, &self.confirmed, r);
                for (_, edit) in &self.pending {
                    if let Edit::Patch(p) = edit {
                        p.apply_within(&mut self.base, r);
                    }
                }
                Region::Rect(r)
            }
            _ => {
                self.base = self.confirmed.clone();
                for (_, edit) in &self.pending {
                    edit.apply(&mut self.base);
                }
                Region::Whole
            }
        }
    }
}
