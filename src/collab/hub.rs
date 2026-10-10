//! The room's coordinator, run by the host kid's app: it puts every edit in one
//! order, passes it on to everyone, and keeps the true picture for newcomers.
//! This is the pure logic; the network thread feeds it messages.

use image::RgbaImage;

use super::patch::Picture;
use super::protocol::{FromHub, Kid, PeerId, ToHub};

/// Who a message is for.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum To {
    One(PeerId),
    All,
    /// Everyone except this kid.
    Others(PeerId),
}

pub struct Hub {
    picture: RgbaImage,
    seq: u64,
    kids: Vec<Kid>,
    /// The kid whose app runs this hub (the first to join it).
    host: Option<PeerId>,
}

impl Hub {
    pub fn new(picture: RgbaImage) -> Self {
        Self { picture, seq: 0, kids: Vec::new(), host: None }
    }

    #[cfg(test)]
    pub fn picture(&self) -> &RgbaImage {
        &self.picture
    }

    /// A kid arrives; `id` is theirs for as long as they stay. `addr` is where
    /// their app could be reached to take over as host.
    pub fn join(&mut self, id: PeerId, name: String, color: [u8; 3], addr: String) -> Vec<(To, FromHub)> {
        self.kids.push(Kid { id, name, color, addr });
        let host = *self.host.get_or_insert(id);
        let welcome = FromHub::Welcome { you: id, host, seq: self.seq, picture: Picture::of(&self.picture), kids: self.kids.clone() };
        vec![(To::One(id), welcome), (To::Others(id), FromHub::Kids(self.kids.clone()))]
    }

    pub fn leave(&mut self, id: PeerId) -> Vec<(To, FromHub)> {
        self.kids.retain(|k| k.id != id);
        vec![(To::All, FromHub::Kids(self.kids.clone()))]
    }

    pub fn handle(&mut self, from: PeerId, msg: ToHub) -> Vec<(To, FromHub)> {
        match msg {
            ToHub::Edit { id, edit } => {
                self.seq += 1;
                edit.apply(&mut self.picture);
                vec![(To::All, FromHub::Sequenced { seq: self.seq, from, id, edit })]
            }
            ToHub::Preview(tiles) => vec![(To::Others(from), FromHub::Preview { from, tiles })],
            ToHub::Presence(presence) => {
                vec![(To::Others(from), FromHub::Presence { from, presence })]
            }
            ToHub::Ping => Vec::new(),
        }
    }
}
