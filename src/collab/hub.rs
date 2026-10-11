//! The room's coordinator, run by the host kid's app: it puts every edit in one
//! order, passes it on to everyone, and keeps the true picture for newcomers.
//! It also keeps the door: newcomers can wait for the host to let them in, and
//! the host can send a kid out. This is the pure logic; the network thread
//! feeds it messages.

use image::RgbaImage;

use super::patch::Picture;
use super::protocol::{FromHub, Kid, PeerId, ToHub};

/// Who a message is for.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum To {
    One(PeerId),
    /// Everyone in the room (not those still waiting to be let in).
    All,
    /// Everyone in the room except this kid.
    Others(PeerId),
    /// Every connection, those waiting to be let in too.
    Everyone,
}

pub struct Hub {
    picture: RgbaImage,
    seq: u64,
    kids: Vec<Kid>,
    /// The kid whose app runs this hub (the first to join it).
    host: Option<PeerId>,
    /// Whether newcomers wait for the host to let them in.
    ask_first: bool,
    /// Kids waiting to be let in, earliest first.
    waiting: Vec<Kid>,
    /// Kids who were in the room before this hub took it over: they come straight back in.
    known: Vec<Kid>,
    /// Where kicked-out kids connected from: coming back, they always wait to be let in.
    kicked: Vec<String>,
}

impl Hub {
    pub fn new(picture: RgbaImage) -> Self {
        let (kids, waiting, known, kicked) = (Vec::new(), Vec::new(), Vec::new(), Vec::new());
        Self { picture, seq: 0, kids, host: None, ask_first: false, waiting, known, kicked }
    }

    /// Whether newcomers wait to be let in, and (taking over a room) who was in it already.
    pub fn with_door(mut self, ask_first: bool, known: Vec<Kid>) -> Self {
        (self.ask_first, self.known) = (ask_first, known);
        self
    }

    #[cfg(test)]
    pub fn picture(&self) -> &RgbaImage {
        &self.picture
    }

    /// Whether this kid is in the room (not just waiting at the door).
    pub fn is_in(&self, id: PeerId) -> bool {
        self.kids.iter().any(|k| k.id == id)
    }

    /// Whether this kid is in the room or waiting to be let in.
    pub fn knows(&self, id: PeerId) -> bool {
        self.is_in(id) || self.waiting.iter().any(|k| k.id == id)
    }

    /// A kid arrives; `id` is theirs for as long as they stay. `addr` is where
    /// their app could be reached to take over as host. They come in, or wait
    /// for the host to let them in.
    pub fn join(&mut self, id: PeerId, name: String, color: [u8; 3], addr: String) -> Vec<(To, FromHub)> {
        let kid = Kid { id, name, color, addr };
        let kicked = self.kicked.iter().any(|ip| *ip == ip_of(&kid.addr));
        let known = self.known.iter().position(|k| k.name == kid.name && ip_of(&k.addr) == ip_of(&kid.addr));
        if let Some(i) = known {
            self.known.remove(i);
        }
        if self.host.is_none() || (!kicked && (!self.ask_first || known.is_some())) {
            return self.admit(kid);
        }
        self.waiting.push(kid);
        let host = self.kids.iter().find(|k| Some(k.id) == self.host).map_or_else(|| "The host".to_owned(), |k| k.name.clone());
        vec![(To::One(id), FromHub::Asking(host)), self.door()]
    }

    fn admit(&mut self, kid: Kid) -> Vec<(To, FromHub)> {
        let id = kid.id;
        self.kids.push(kid);
        let host = *self.host.get_or_insert(id);
        let welcome = FromHub::Welcome {
            you: id,
            host,
            seq: self.seq,
            picture: Picture::of(&self.picture),
            kids: self.kids.clone(),
            ask_first: self.ask_first,
        };
        vec![(To::One(id), welcome), (To::Others(id), FromHub::Kids(self.kids.clone()))]
    }

    /// Tell the host who's waiting at the door.
    fn door(&self) -> (To, FromHub) {
        (To::One(self.host.unwrap_or_default()), FromHub::Waiting(self.waiting.clone()))
    }

    pub fn leave(&mut self, id: PeerId) -> Vec<(To, FromHub)> {
        if let Some(i) = self.waiting.iter().position(|k| k.id == id) {
            self.waiting.remove(i);
            return vec![self.door()];
        }
        if !self.is_in(id) {
            return Vec::new();
        }
        self.kids.retain(|k| k.id != id);
        vec![(To::All, FromHub::Kids(self.kids.clone()))]
    }

    pub fn handle(&mut self, from: PeerId, msg: ToHub) -> Vec<(To, FromHub)> {
        if !self.is_in(from) {
            return Vec::new();
        }
        let host = self.host == Some(from);
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
            ToHub::Kick(kid) if host && kid != from => {
                let Some(i) = self.kids.iter().position(|k| k.id == kid) else {
                    return Vec::new();
                };
                let gone = self.kids.remove(i);
                self.kicked.push(ip_of(&gone.addr).to_owned());
                let news = FromHub::KickedOut { kid, by: from, kids: self.kids.clone() };
                vec![(To::One(kid), news.clone()), (To::All, news)]
            }
            ToHub::LetIn { kid, yes } if host => {
                let Some(i) = self.waiting.iter().position(|k| k.id == kid) else {
                    return Vec::new();
                };
                let newcomer = self.waiting.remove(i);
                let mut out = if yes {
                    self.admit(newcomer)
                } else {
                    let name = self.kids.iter().find(|k| k.id == from).map_or("The host", |k| k.name.as_str());
                    vec![(To::One(kid), FromHub::Rejected(format!("{name} didn't let you in this time.")))]
                };
                out.push(self.door());
                out
            }
            ToHub::AskFirst(on) if host => {
                self.ask_first = on;
                let mut out = vec![(To::All, FromHub::AskFirst(on))];
                if !on {
                    // Everyone waiting who'd now get in without asking comes in.
                    let (come_in, wait): (Vec<Kid>, Vec<Kid>) =
                        self.waiting.drain(..).partition(|k| !self.kicked.iter().any(|ip| *ip == ip_of(&k.addr)));
                    self.waiting = wait;
                    for kid in come_in {
                        out.extend(self.admit(kid));
                    }
                    out.push(self.door());
                }
                out
            }
            ToHub::Kick(_) | ToHub::LetIn { .. } | ToHub::AskFirst(_) => Vec::new(),
        }
    }
}

/// The computer part of "ip:port".
fn ip_of(addr: &str) -> &str {
    addr.rsplit_once(':').map_or(addr, |(ip, _)| ip)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn hub(ask_first: bool) -> Hub {
        let mut hub = Hub::new(RgbaImage::new(8, 8)).with_door(ask_first, Vec::new());
        hub.join(1, "Host".into(), [0; 3], "10.0.0.1:47100".into());
        hub
    }

    fn welcomed(out: &[(To, FromHub)], id: PeerId) -> bool {
        out.iter().any(|(to, msg)| *to == To::One(id) && matches!(msg, FromHub::Welcome { .. }))
    }

    #[test]
    fn a_kicked_kid_has_to_ask_to_come_back() {
        let mut hub = hub(false);
        assert!(welcomed(&hub.join(2, "Fox".into(), [0; 3], "10.0.0.2:5000".into()), 2));
        let out = hub.handle(1, ToHub::Kick(2));
        assert!(matches!(&out[0], (To::One(2), FromHub::KickedOut { kid: 2, by: 1, kids }) if kids.len() == 1));
        assert!(!hub.knows(2));
        // From the same computer, under any name: they wait for the host.
        let out = hub.join(3, "Not Fox".into(), [0; 3], "10.0.0.2:5001".into());
        assert_eq!(out[0], (To::One(3), FromHub::Asking("Host".into())));
        assert!(matches!(&out[1], (To::One(1), FromHub::Waiting(w)) if w.len() == 1));
        // Anyone else still comes straight in.
        assert!(welcomed(&hub.join(4, "Owl".into(), [0; 3], "10.0.0.4:5000".into()), 4));
    }

    #[test]
    fn only_the_host_kicks_and_lets_in() {
        let mut hub = hub(true);
        hub.join(2, "Fox".into(), [0; 3], "10.0.0.2:5000".into());
        hub.handle(1, ToHub::LetIn { kid: 2, yes: true });
        hub.join(3, "Owl".into(), [0; 3], "10.0.0.3:5000".into());
        assert!(hub.handle(2, ToHub::LetIn { kid: 3, yes: true }).is_empty());
        assert!(hub.handle(2, ToHub::Kick(1)).is_empty());
        assert!(hub.handle(1, ToHub::Kick(1)).is_empty(), "not the host themselves");
        assert!(hub.handle(3, ToHub::Ping).is_empty() && !hub.is_in(3), "still waiting");
        let out = hub.handle(1, ToHub::LetIn { kid: 3, yes: false });
        assert_eq!(out[0], (To::One(3), FromHub::Rejected("Host didn't let you in this time.".into())));
        assert!(!hub.knows(3));
    }

    #[test]
    fn letting_friends_in_without_asking_lets_in_those_waiting() {
        let mut hub = hub(true);
        hub.join(2, "Fox".into(), [0; 3], "10.0.0.2:5000".into());
        hub.join(3, "Owl".into(), [0; 3], "10.0.0.3:5000".into());
        hub.leave(3);
        let out = hub.handle(1, ToHub::AskFirst(false));
        assert!(welcomed(&out, 2) && hub.is_in(2) && !hub.knows(3));
        assert_eq!(out.last(), Some(&(To::One(1), FromHub::Waiting(Vec::new()))));
    }

    #[test]
    fn after_a_takeover_everyone_who_was_in_comes_straight_back() {
        let kid = |id, name: &str, addr: &str| Kid { id, name: name.into(), color: [0; 3], addr: addr.into() };
        let known = vec![kid(1, "Host", "10.0.0.1:47100"), kid(2, "Fox", "10.0.0.2:5000"), kid(3, "Owl", "10.0.0.3:5000")];
        let mut hub = Hub::new(RgbaImage::new(8, 8)).with_door(true, known);
        assert!(welcomed(&hub.join(1, "Fox".into(), [0; 3], "10.0.0.2:5000".into()), 1));
        assert!(welcomed(&hub.join(2, "Host".into(), [0; 3], "10.0.0.1:47101".into()), 2));
        assert!(matches!(hub.join(3, "Newt".into(), [0; 3], "10.0.0.9:5000".into())[0], (To::One(3), FromHub::Asking(_))));
        assert!(matches!(hub.join(4, "Owl".into(), [0; 3], "10.0.0.8:5000".into())[0], (To::One(4), FromHub::Asking(_))));
        let out = hub.join(5, "Owl".into(), [0; 3], "10.0.0.3:6000".into());
        assert!(welcomed(&out, 5));
        assert!(matches!(&out[0], (_, FromHub::Welcome { ask_first: true, .. })), "the rule carries on");
    }
}
