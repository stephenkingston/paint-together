//! What the kids' apps say to the host's hub, and what it says back.

use super::patch::{Edit, Patch, Picture};
use crate::raster::IRect;

pub type PeerId = u32;

/// Bumped whenever the messages change; apps only talk to the same version.
pub const VERSION: u32 = 1;

/// The first thing a kid's app sends after connecting.
#[derive(Clone, Debug, PartialEq, bincode::Encode, bincode::Decode)]
pub struct Hello {
    /// Always `*b"PNT!"`, so stray connections are turned away.
    pub magic: [u8; 4],
    pub version: u32,
    pub name: String,
    pub color: [u8; 3],
    /// The port this app keeps ready, to take over as host if the host leaves.
    pub listen: u16,
}

impl Hello {
    pub const MAGIC: [u8; 4] = *b"PNT!";

    pub fn new(name: String, color: [u8; 3], listen: u16) -> Self {
        Self { magic: Self::MAGIC, version: VERSION, name, color, listen }
    }
}

/// Someone in the room.
#[derive(Clone, Debug, PartialEq, bincode::Encode, bincode::Decode)]
pub struct Kid {
    pub id: PeerId,
    pub name: String,
    pub color: [u8; 3],
    /// Where their app can be reached if it takes over as host ("ip:port").
    pub addr: String,
}

/// Part of a drawing still in progress: the pixels of one 64×64 tile of the
/// picture that differ from the shared picture (none: the tile is back to normal).
#[derive(Clone, Debug, PartialEq, bincode::Encode, bincode::Decode)]
pub struct Tile {
    pub tx: u16,
    pub ty: u16,
    pub patch: Option<Patch>,
}

/// Side length of a preview tile.
pub const TILE: i32 = 64;

/// Where a kid's pointer is and what they're holding.
#[derive(Clone, Copy, Debug, PartialEq, bincode::Encode, bincode::Decode)]
pub struct Presence {
    /// In picture pixels; none when the pointer is off the picture.
    pub at: Option<(f32, f32)>,
    pub tool: Held,
    /// Color 1 (for the eraser, Color 2: what it paints with).
    pub color: [u8; 4],
    /// The eraser's size, in picture pixels.
    pub size: f32,
}

/// The tool a kid is holding, for drawing their pointer.
#[derive(Clone, Copy, Debug, PartialEq, Eq, bincode::Encode, bincode::Decode)]
pub enum Held {
    Pencil,
    Fill,
    Brush,
    Eraser,
    Picker,
    Other,
}

/// From a kid's app to the hub.
#[derive(Clone, Debug, PartialEq, bincode::Encode, bincode::Decode)]
pub enum ToHub {
    /// A finished change; `id` counts up per sender so the echo can be matched.
    Edit {
        id: u64,
        edit: Edit,
    },
    /// What I'm drawing right now (passed on to everyone else, not kept).
    Preview(Vec<Tile>),
    Presence(Presence),
    /// Still here (sent when there's nothing else to say).
    Ping,
}

/// From the hub to the kids' apps.
#[derive(Clone, Debug, PartialEq, bincode::Encode, bincode::Decode)]
pub enum FromHub {
    /// You're in: here's the picture as of edit number `seq`. `host` is the kid
    /// whose app runs the room.
    Welcome {
        you: PeerId,
        host: PeerId,
        seq: u64,
        picture: Picture,
        kids: Vec<Kid>,
    },
    /// An edit in its final place in the order everyone applies them in
    /// (sent to its author too, as confirmation).
    Sequenced {
        seq: u64,
        from: PeerId,
        id: u64,
        edit: Edit,
    },
    /// Who's in the room now.
    Kids(Vec<Kid>),
    /// Not let in (a different version of Paint, say).
    Rejected(String),
    /// What a friend is drawing right now; their finished edit replaces it.
    Preview {
        from: PeerId,
        tiles: Vec<Tile>,
    },
    Presence {
        from: PeerId,
        presence: Presence,
    },
    /// Still here.
    Ping,
}

impl ToHub {
    pub fn is_valid(&self) -> bool {
        match self {
            ToHub::Edit { edit, .. } => edit.is_valid(),
            ToHub::Preview(tiles) => tiles_valid(tiles),
            ToHub::Presence(p) => p.is_valid(),
            ToHub::Ping => true,
        }
    }
}

impl FromHub {
    pub fn is_valid(&self) -> bool {
        match self {
            FromHub::Welcome { picture, .. } => picture.is_valid(),
            FromHub::Sequenced { edit, .. } => edit.is_valid(),
            FromHub::Preview { tiles, .. } => tiles_valid(tiles),
            FromHub::Presence { presence, .. } => presence.is_valid(),
            FromHub::Kids(_) | FromHub::Rejected(_) | FromHub::Ping => true,
        }
    }
}

impl Tile {
    /// The picture pixels this tile covers.
    pub fn rect(tx: u16, ty: u16) -> IRect {
        let (x, y) = (tx as i32 * TILE, ty as i32 * TILE);
        IRect::new(x, y, x + TILE, y + TILE)
    }
}

fn tiles_valid(tiles: &[Tile]) -> bool {
    tiles.len() <= 4096
        && tiles.iter().all(|t| t.patch.as_ref().is_none_or(|p| p.is_valid() && Tile::rect(t.tx, t.ty).contains_rect(p.rect())))
}

impl Presence {
    fn is_valid(&self) -> bool {
        self.at.is_none_or(|(x, y)| x.is_finite() && y.is_finite()) && (0.0..=1000.0).contains(&self.size)
    }
}
