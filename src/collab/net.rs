//! Carrying messages between the kids' apps over the local network.
//!
//! Each message is one frame: a 4-byte length, a flag byte (1 = deflated) and the
//! `bincode` body. Everything that arrives is size-limited and checked before use.
//!
//! The host's app runs the hub on its own thread, with a listener that lets
//! friends connect; the host's own drawing talks to that hub through a channel,
//! so the host and its friends run exactly the same code.

use std::collections::HashMap;
use std::io::{self, BufReader, BufWriter, Read, Write};
use std::net::{Shutdown, SocketAddr, TcpListener, TcpStream, ToSocketAddrs, UdpSocket};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::mpsc::{self, Receiver, Sender};
use std::thread;
use std::time::{Duration, Instant};

use bincode::{Decode, Encode};
use eframe::egui::Context;
use image::RgbaImage;

use super::hub::{Hub, To};
use super::protocol::{FromHub, Hello, PeerId, ToHub, VERSION};

/// The port a hosted drawing listens on (another is picked if it's taken).
pub const PORT: u16 = 47100;
/// The largest frame accepted, before or after decompression.
const MAX_FRAME: usize = 128 << 20;
/// Bodies at least this big are deflated.
const COMPRESS_FROM: usize = 2048;

fn config() -> impl bincode::config::Config {
    bincode::config::standard().with_limit::<MAX_FRAME>()
}

pub fn write_frame<T: Encode>(w: &mut impl Write, msg: &T) -> io::Result<()> {
    let raw = bincode::encode_to_vec(msg, config()).map_err(io::Error::other)?;
    let (flag, body) = if raw.len() >= COMPRESS_FROM {
        let mut z = flate2::write::DeflateEncoder::new(Vec::new(), flate2::Compression::fast());
        z.write_all(&raw)?;
        (1u8, z.finish()?)
    } else {
        (0u8, raw)
    };
    w.write_all(&(body.len() as u32 + 1).to_be_bytes())?;
    w.write_all(&[flag])?;
    w.write_all(&body)?;
    w.flush()
}

pub fn read_frame<T: Decode<()>>(r: &mut impl Read) -> io::Result<T> {
    let mut len = [0u8; 4];
    r.read_exact(&mut len)?;
    let len = u32::from_be_bytes(len) as usize;
    if len == 0 || len > MAX_FRAME {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "frame too large"));
    }
    let mut frame = vec![0u8; len];
    r.read_exact(&mut frame)?;
    let raw = match frame[0] {
        0 => frame.split_off(1),
        1 => {
            let mut raw = Vec::new();
            flate2::read::DeflateDecoder::new(&frame[1..]).take(MAX_FRAME as u64 + 1).read_to_end(&mut raw)?;
            if raw.len() > MAX_FRAME {
                return Err(io::Error::new(io::ErrorKind::InvalidData, "frame too large"));
            }
            raw
        }
        _ => return Err(io::Error::new(io::ErrorKind::InvalidData, "unknown frame")),
    };
    let (msg, used) = bincode::decode_from_slice(&raw, config()).map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))?;
    if used != raw.len() {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "trailing bytes"));
    }
    Ok(msg)
}

/// What the network tells the app.
#[derive(Debug)]
pub enum LinkEvent {
    Message(FromHub),
    /// The connection is gone; the reason is for the kid to read.
    Closed(String),
}

/// The app's connection to its room.
pub struct Link {
    up: Up,
    events: Receiver<LinkEvent>,
    /// Set when hosting: stops the hub and the listener when the link goes away.
    host: Option<HostHandle>,
    /// When joined: the port kept ready to take over as host if the host leaves.
    spare: Option<TcpListener>,
}

enum Up {
    /// The host's own app, talking to its hub thread.
    Local(Sender<HubEvent>, PeerId),
    /// A friend's app, talking over TCP through a writer thread. Dropping the
    /// sender lets that thread send what's queued, then hang up.
    Remote(Sender<ToHub>),
}

/// How long a quiet connection waits before saying it's still there, and how
/// long without hearing anything means the other side is gone.
const PING_EVERY: Duration = if cfg!(test) { Duration::from_millis(100) } else { Duration::from_secs(2) };
const GIVE_UP_AFTER: Duration = if cfg!(test) { Duration::from_millis(400) } else { Duration::from_secs(6) };

/// Where listeners listen: the whole local network (tests keep to this computer).
fn bind_ip() -> std::net::Ipv4Addr {
    if cfg!(test) { std::net::Ipv4Addr::LOCALHOST } else { std::net::Ipv4Addr::UNSPECIFIED }
}

/// A listener on the usual port, or any free one.
pub fn listen() -> io::Result<TcpListener> {
    TcpListener::bind((bind_ip(), PORT)).or_else(|_| TcpListener::bind((bind_ip(), 0)))
}

impl Link {
    /// Start a room with `picture`, listening for friends on the local network.
    /// With `ask_first`, newcomers wait for me to let them in.
    pub fn host(picture: RgbaImage, name: String, color: [u8; 3], ask_first: bool, ctx: Context) -> io::Result<Link> {
        Self::host_with(listen()?, Hub::new(picture).with_door(ask_first, Vec::new()), name, color, ctx)
    }

    /// Run `hub` on a listener that's already open (taking over as host).
    pub fn host_with(listener: TcpListener, hub: Hub, name: String, color: [u8; 3], ctx: Context) -> io::Result<Link> {
        let port = listener.local_addr()?.port();
        let (hub_tx, hub_rx) = mpsc::channel();
        let ids = Arc::new(AtomicU32::new(1));
        let stop = Arc::new(AtomicBool::new(false));

        thread::Builder::new().name("paint-hub".into()).spawn(move || run_hub(hub, hub_rx))?;
        {
            let (hub_tx, ids, stop, ctx) = (hub_tx.clone(), ids.clone(), stop.clone(), ctx.clone());
            thread::Builder::new().name("paint-listen".into()).spawn(move || {
                for stream in listener.incoming() {
                    if stop.load(Ordering::Relaxed) {
                        break;
                    }
                    let Ok(stream) = stream else { continue };
                    let (hub_tx, id) = (hub_tx.clone(), ids.fetch_add(1, Ordering::Relaxed));
                    let ctx = ctx.clone();
                    let _ = thread::Builder::new().name(format!("paint-kid-{id}")).spawn(move || serve_kid(stream, id, hub_tx, ctx));
                }
            })?;
        }

        let me = ids.fetch_add(1, Ordering::Relaxed);
        let (events_tx, events) = mpsc::channel();
        let addr = SocketAddr::new(lan_ip().unwrap_or([127, 0, 0, 1].into()), port).to_string();
        hub_tx
            .send(HubEvent::Join { id: me, name, color, addr, sink: Sink::Local(events_tx, ctx) })
            .map_err(|_| io::Error::other("the hub stopped"))?;
        let host = HostHandle { hub: hub_tx.clone(), stop, port };
        Ok(Link { up: Up::Local(hub_tx, me), events, host: Some(host), spare: None })
    }

    /// Join a friend's room. Connecting happens in the background; the result
    /// arrives as a `Welcome` (or `Closed`) event. `spare` is a listener to keep
    /// ready for taking over as host (a new one is opened if none is given).
    pub fn join(addr: String, name: String, color: [u8; 3], spare: Option<TcpListener>, ctx: Context) -> io::Result<Link> {
        let spare = match spare {
            Some(l) => l,
            None => listen()?,
        };
        let hello = Hello::new(name, color, spare.local_addr()?.port());
        let (up_tx, up_rx) = mpsc::channel::<ToHub>();
        let (events_tx, events) = mpsc::channel();
        thread::Builder::new().name("paint-receive".into()).spawn(move || {
            let reason = match connect(&addr, hello, up_rx) {
                Ok(stream) => receive(stream, &events_tx, &ctx),
                Err(why) => why,
            };
            let _ = events_tx.send(LinkEvent::Closed(reason));
            ctx.request_repaint();
        })?;
        Ok(Link { up: Up::Remote(up_tx), events, host: None, spare: Some(spare) })
    }

    pub fn send(&self, msg: ToHub) {
        match &self.up {
            Up::Local(hub, me) => _ = hub.send(HubEvent::Message { from: *me, msg }),
            Up::Remote(tx) => _ = tx.send(msg),
        }
    }

    pub fn try_recv(&self) -> Option<LinkEvent> {
        self.events.try_recv().ok()
    }

    /// When hosting: where friends can reach this drawing.
    pub fn hosting_at(&self) -> Option<SocketAddr> {
        let port = self.host.as_ref()?.port;
        Some(SocketAddr::new(lan_ip().unwrap_or([127, 0, 0, 1].into()), port))
    }

    /// The listener kept ready for taking over as host.
    pub fn take_spare(&mut self) -> Option<TcpListener> {
        self.spare.take()
    }
}

/// Connect, say hello, and start the thread that sends my messages (and a ping
/// whenever there's been nothing to send for a while).
fn connect(addr: &str, hello: Hello, up: Receiver<ToHub>) -> Result<TcpStream, String> {
    let addr = resolve(addr).map_err(|e| e.to_string())?;
    let unreachable = || format!("Couldn't reach a drawing at {addr}. Check the address, and that you're on the same Wi-Fi.");
    let stream = TcpStream::connect_timeout(&addr, Duration::from_secs(4)).map_err(|_| unreachable())?;
    let _ = stream.set_nodelay(true);
    let mut w = BufWriter::new(stream.try_clone().map_err(|_| unreachable())?);
    write_frame(&mut w, &hello).map_err(|_| unreachable())?;
    thread::Builder::new()
        .name("paint-send".into())
        .spawn(move || {
            loop {
                let msg = match up.recv_timeout(PING_EVERY) {
                    Ok(msg) => msg,
                    Err(mpsc::RecvTimeoutError::Timeout) => ToHub::Ping,
                    Err(mpsc::RecvTimeoutError::Disconnected) => break,
                };
                if write_frame(&mut w, &msg).is_err() {
                    break;
                }
            }
            let _ = w.get_ref().shutdown(Shutdown::Both);
        })
        .map_err(|e| e.to_string())?;
    Ok(stream)
}

/// Pass the hub's messages to the app until the connection ends; returns why it did.
fn receive(stream: TcpStream, events: &Sender<LinkEvent>, ctx: &Context) -> String {
    let _ = stream.set_read_timeout(Some(GIVE_UP_AFTER));
    let mut r = BufReader::new(stream);
    loop {
        match read_frame::<FromHub>(&mut r) {
            Ok(FromHub::Ping) => {}
            Ok(msg) if msg.is_valid() => {
                let rejected = matches!(msg, FromHub::Rejected(_));
                if events.send(LinkEvent::Message(msg)).is_err() || rejected {
                    return String::new();
                }
                ctx.request_repaint();
            }
            Ok(_) => return "Your friend's Paint sent something this one can't use.".to_owned(),
            Err(_) => {
                return "The drawing ended: the host left, or the network dropped.".to_owned();
            }
        }
    }
}

impl Drop for Link {
    fn drop(&mut self) {
        if let Some(host) = &self.host {
            host.stop.store(true, Ordering::Relaxed);
            let _ = host.hub.send(HubEvent::Stop);
            // Wake the listener so it notices.
            let _ = TcpStream::connect_timeout(&([127, 0, 0, 1], host.port).into(), Duration::from_millis(200));
        }
    }
}

struct HostHandle {
    hub: Sender<HubEvent>,
    stop: Arc<AtomicBool>,
    port: u16,
}

/// Where the hub sends a kid's messages.
enum Sink {
    Local(Sender<LinkEvent>, Context),
    Remote(Sender<FromHub>),
}

enum HubEvent {
    Join { id: PeerId, name: String, color: [u8; 3], addr: String, sink: Sink },
    Message { from: PeerId, msg: ToHub },
    Left(PeerId),
    Stop,
}

/// The hub's thread: one event at a time, so every edit gets one place in the order.
fn run_hub(mut hub: Hub, events: Receiver<HubEvent>) {
    let mut sinks: HashMap<PeerId, Sink> = HashMap::new();
    let mut next_ping = Instant::now() + PING_EVERY;
    loop {
        let mut out = match events.recv_timeout(next_ping.saturating_duration_since(Instant::now())) {
            Ok(HubEvent::Join { id, name, color, addr, sink }) => {
                sinks.insert(id, sink);
                hub.join(id, name, color, addr)
            }
            Ok(HubEvent::Message { from, msg }) if sinks.contains_key(&from) => hub.handle(from, msg),
            Ok(HubEvent::Left(id)) if sinks.remove(&id).is_some() => hub.leave(id),
            Ok(HubEvent::Message { .. } | HubEvent::Left(_)) | Err(mpsc::RecvTimeoutError::Timeout) => Vec::new(),
            Ok(HubEvent::Stop) | Err(mpsc::RecvTimeoutError::Disconnected) => break,
        };
        // Everyone hears from the room regularly, however busy it is with
        // others, so a quiet connection (or one waiting at the door) isn't
        // mistaken for a lost one.
        if Instant::now() >= next_ping {
            out.push((To::Everyone, FromHub::Ping));
            next_ping = Instant::now() + PING_EVERY;
        }
        while !out.is_empty() {
            let mut gone = Vec::new();
            for (&id, sink) in &sinks {
                let mut mine = out.iter().filter(|(to, _)| match *to {
                    To::Everyone => true,
                    To::One(k) => k == id,
                    To::All => hub.is_in(id),
                    To::Others(k) => k != id && hub.is_in(id),
                });
                if !mine.all(|(_, msg)| deliver(sink, msg)) {
                    gone.push(id);
                }
            }
            // Those sent away (or not let in) hang up once their news is on its way.
            sinks.retain(|id, _| hub.knows(*id) && !gone.contains(id));
            out = gone.into_iter().flat_map(|id| hub.leave(id)).collect();
        }
    }
}

/// Pass one of the hub's messages on; false when that kid's connection is gone.
fn deliver(sink: &Sink, msg: &FromHub) -> bool {
    match sink {
        Sink::Local(_, _) if *msg == FromHub::Ping => true,
        Sink::Local(tx, ctx) => {
            let ok = tx.send(LinkEvent::Message(msg.clone())).is_ok();
            ctx.request_repaint();
            ok
        }
        Sink::Remote(tx) => tx.send(msg.clone()).is_ok(),
    }
}

/// One friend's connection, on the host: say hello, then pass messages both ways.
fn serve_kid(stream: TcpStream, id: PeerId, hub: Sender<HubEvent>, ctx: Context) {
    let _ = stream.set_nodelay(true);
    let Ok(read_half) = stream.try_clone() else {
        return;
    };
    let mut r = BufReader::new(read_half);
    let mut w = BufWriter::new(stream);
    let _ = r.get_ref().set_read_timeout(Some(Duration::from_secs(5)));
    let hello = match read_frame::<Hello>(&mut r) {
        Ok(h) if h.magic == Hello::MAGIC => h,
        _ => return,
    };
    if hello.version != VERSION {
        let why = "Your friend has a different version of Paint. Use the same version to draw together.";
        let _ = write_frame(&mut w, &FromHub::Rejected(why.into()));
        return;
    }
    let _ = r.get_ref().set_read_timeout(Some(GIVE_UP_AFTER));
    let name: String = hello.name.chars().take(24).collect();
    // Where this kid's app could be reached, should it take over as host.
    let addr = r.get_ref().peer_addr().map(|a| SocketAddr::new(a.ip(), hello.listen).to_string()).unwrap_or_default();

    let (tx, rx) = mpsc::channel::<FromHub>();
    if hub.send(HubEvent::Join { id, name, color: hello.color, addr, sink: Sink::Remote(tx) }).is_err() {
        return;
    }
    let Ok(socket) = w.get_ref().try_clone() else {
        return;
    };
    thread::spawn(move || {
        for msg in rx {
            if write_frame(&mut w, &msg).is_err() {
                break;
            }
        }
        let _ = w.get_ref().shutdown(Shutdown::Both);
    });
    loop {
        match read_frame::<ToHub>(&mut r) {
            Ok(msg) if msg.is_valid() => {
                if hub.send(HubEvent::Message { from: id, msg }).is_err() {
                    break;
                }
            }
            _ => break,
        }
    }
    let _ = socket.shutdown(Shutdown::Both);
    let _ = hub.send(HubEvent::Left(id));
    ctx.request_repaint();
}

/// "192.168.1.4", "192.168.1.4:47100" or a host name.
fn resolve(addr: &str) -> io::Result<SocketAddr> {
    let addr = addr.trim();
    let with_port = if addr.contains(':') { addr.to_owned() } else { format!("{addr}:{PORT}") };
    with_port.to_socket_addrs()?.find(|a| a.is_ipv4()).ok_or_else(|| io::Error::other(format!("Couldn't find \"{addr}\" on the network.")))
}

/// This computer's address on the local network (no packets are sent).
pub fn lan_ip() -> Option<std::net::IpAddr> {
    let socket = UdpSocket::bind("0.0.0.0:0").ok()?;
    socket.connect("192.0.2.1:9").ok()?;
    socket.local_addr().ok().map(|a| a.ip()).filter(|ip| !ip.is_unspecified())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::collab::patch::{Edit, Patch, Picture};
    use crate::collab::protocol::Presence;
    use crate::raster::IRect;

    #[test]
    fn frames_round_trip_and_big_ones_are_compressed() {
        let pic = Picture { w: 300, h: 200, rgba: vec![255; 300 * 200 * 4] };
        let msg = FromHub::Welcome { you: 3, host: 1, seq: 9, picture: pic, kids: vec![], ask_first: false };
        let mut buf = Vec::new();
        write_frame(&mut buf, &msg).unwrap();
        assert!(buf.len() < 10_000, "a white picture compresses well: {} bytes", buf.len());
        assert_eq!(read_frame::<FromHub>(&mut buf.as_slice()).unwrap(), msg);

        let patch = Patch::build(IRect::new(5, 5, 9, 7), |x, _| (x % 2 == 0).then_some([1, 2, 3, 4])).unwrap();
        let msg = ToHub::Edit { id: 1, edit: Edit::Patch(patch) };
        let mut buf = Vec::new();
        write_frame(&mut buf, &msg).unwrap();
        assert_eq!(read_frame::<ToHub>(&mut buf.as_slice()).unwrap(), msg);
    }

    #[test]
    fn bad_frames_are_refused() {
        // Claims to be enormous.
        assert!(read_frame::<ToHub>(&mut [0xFF, 0xFF, 0xFF, 0xFF, 0].as_slice()).is_err());
        // Garbage body.
        assert!(read_frame::<ToHub>(&mut [0, 0, 0, 3, 0, 9, 9].as_slice()).is_err());
        // A patch whose pixels don't match its mask decodes, but isn't valid.
        let bad = Patch { x: 0, y: 0, w: 4, h: 4, mask: vec![0xFF, 0xFF], pixels: vec![0; 8] };
        assert!(!ToHub::Edit { id: 1, edit: Edit::Patch(bad) }.is_valid());
        let bad = Picture { w: 10, h: 10, rgba: vec![0; 12] };
        assert!(!Edit::Replace(bad).is_valid());
    }

    /// By hand, across two computers: start a drawing in Paint on one, then on
    /// the other run `PAINT_JOIN=<its address> cargo test joins_a_real_room --
    /// --ignored --nocapture`. A blue square should appear in the drawing.
    #[test]
    #[ignore]
    fn joins_a_real_room() {
        let addr = std::env::var("PAINT_JOIN").expect("set PAINT_JOIN to the drawing's address");
        let link = Link::join(addr, "Test Penguin".into(), [0, 150, 199], None, Context::default()).unwrap();
        let next = || loop {
            match link.try_recv() {
                Some(LinkEvent::Message(m)) => return m,
                Some(LinkEvent::Closed(why)) => panic!("closed: {why}"),
                None => thread::sleep(Duration::from_millis(10)),
            }
        };
        let FromHub::Welcome { you, picture, kids, .. } = next() else { panic!("expected a welcome") };
        let names: Vec<&str> = kids.iter().map(|k| k.name.as_str()).collect();
        println!("joined as kid {you}: a {}x{} picture, with {names:?}", picture.w, picture.h);
        let square = Patch::build(IRect::new(40, 40, 140, 140), |_, _| Some([0, 150, 199, 255])).unwrap();
        link.send(ToHub::Edit { id: 1, edit: Edit::Patch(square) });
        loop {
            if let FromHub::Sequenced { seq, from, id: 1, .. } = next() {
                if from == you {
                    println!("the square was confirmed as edit {seq}");
                    break;
                }
            }
        }
        thread::sleep(Duration::from_secs(4));
    }

    #[test]
    fn a_quiet_friend_stays_connected_while_another_chats() {
        let ctx = Context::default();
        let white = RgbaImage::from_pixel(32, 32, image::Rgba([255, 255, 255, 255]));
        let host = Link::host(white, "Host".into(), [1, 2, 3], false, ctx.clone()).unwrap();
        let addr = format!("127.0.0.1:{}", host.hosting_at().unwrap().port());
        let chatty = Link::join(addr.clone(), "Chatty".into(), [4, 5, 6], None, ctx.clone()).unwrap();
        let quiet = Link::join(addr, "Quiet".into(), [7, 8, 9], None, ctx).unwrap();
        // For several times the give-up time, one friend keeps the hub busy while
        // the other sends nothing of its own and has nothing new to hear.
        let presence = Presence { at: Some((1.0, 1.0)), tool: super::super::protocol::Held::Other, color: [0; 4], size: 0.0 };
        let until = Instant::now() + GIVE_UP_AFTER * 5;
        while Instant::now() < until {
            chatty.send(ToHub::Presence(presence));
            thread::sleep(Duration::from_millis(20));
            for link in [&host, &chatty, &quiet] {
                while let Some(ev) = link.try_recv() {
                    assert!(!matches!(ev, LinkEvent::Closed(_)), "a connection dropped: {ev:?}");
                }
            }
        }
    }

    /// Wait for the next message on `link` that `want` picks out, skipping others.
    fn wait_for<T>(link: &Link, mut want: impl FnMut(FromHub) -> Option<T>) -> T {
        let until = Instant::now() + Duration::from_secs(5);
        while Instant::now() < until {
            match link.try_recv() {
                Some(LinkEvent::Message(m)) => {
                    if let Some(t) = want(m) {
                        return t;
                    }
                }
                Some(LinkEvent::Closed(why)) => panic!("closed: {why}"),
                None => thread::sleep(Duration::from_millis(5)),
            }
        }
        panic!("it never arrived");
    }

    #[test]
    fn a_friend_waits_at_the_door_until_let_in_and_hangs_up_when_kicked() {
        let ctx = Context::default();
        let white = RgbaImage::from_pixel(32, 32, image::Rgba([255, 255, 255, 255]));
        let host = Link::host(white, "Host".into(), [1, 2, 3], true, ctx.clone()).unwrap();
        let addr = format!("127.0.0.1:{}", host.hosting_at().unwrap().port());
        let friend = Link::join(addr, "Friend".into(), [4, 5, 6], None, ctx).unwrap();
        let host_id = wait_for(&host, |m| if let FromHub::Welcome { you, .. } = m { Some(you) } else { None });
        assert_eq!(wait_for(&friend, |m| if let FromHub::Asking(h) = m { Some(h) } else { None }), "Host");
        let waiting = wait_for(&host, |m| if let FromHub::Waiting(w) = m { Some(w) } else { None });
        assert_eq!(waiting.len(), 1);
        let friend_id = waiting[0].id;

        // Waiting for longer than a quiet connection lasts, hearing nothing of the drawing.
        let patch = Patch::build(IRect::new(1, 1, 3, 3), |_, _| Some([9, 9, 9, 255])).unwrap();
        host.send(ToHub::Edit { id: 1, edit: Edit::Patch(patch) });
        let until = Instant::now() + GIVE_UP_AFTER * 2;
        while Instant::now() < until {
            match friend.try_recv() {
                Some(LinkEvent::Closed(why)) => panic!("dropped while waiting: {why}"),
                Some(LinkEvent::Message(m)) => panic!("heard {m:?} before being let in"),
                None => thread::sleep(Duration::from_millis(10)),
            }
        }

        host.send(ToHub::LetIn { kid: friend_id, yes: true });
        let seq = wait_for(&friend, |m| if let FromHub::Welcome { seq, .. } = m { Some(seq) } else { None });
        assert_eq!(seq, 1, "the picture so far comes with the welcome");

        host.send(ToHub::Kick(friend_id));
        let by = wait_for(&friend, |m| if let FromHub::KickedOut { by, .. } = m { Some(by) } else { None });
        assert_eq!(by, host_id);
        let until = Instant::now() + Duration::from_secs(5);
        while Instant::now() < until {
            if let Some(LinkEvent::Closed(_)) = friend.try_recv() {
                return;
            }
            thread::sleep(Duration::from_millis(10));
        }
        panic!("the kicked friend's connection stayed open");
    }

    #[test]
    fn host_and_friend_over_real_sockets() {
        let ctx = Context::default();
        let white = RgbaImage::from_pixel(64, 48, image::Rgba([255, 255, 255, 255]));
        let host = Link::host(white, "Host".into(), [1, 2, 3], false, ctx.clone()).unwrap();
        let addr = host.hosting_at().unwrap();
        let friend = Link::join(format!("127.0.0.1:{}", addr.port()), "Friend".into(), [4, 5, 6], None, ctx).unwrap();

        let next = |link: &Link| -> FromHub {
            for _ in 0..500 {
                if let Some(ev) = link.try_recv() {
                    match ev {
                        LinkEvent::Message(m) => return m,
                        LinkEvent::Closed(why) => panic!("closed: {why}"),
                    }
                }
                thread::sleep(Duration::from_millis(10));
            }
            panic!("nothing arrived");
        };
        let FromHub::Welcome { you: host_id, .. } = next(&host) else { panic!() };
        let FromHub::Welcome { you: friend_id, host: hosted_by, picture, kids, .. } = next(&friend) else { panic!() };
        assert_eq!((picture.w, picture.h), (64, 48));
        assert_eq!(kids.len(), 2);
        assert_eq!(hosted_by, host_id);
        let friend_kid = kids.iter().find(|k| k.id == friend_id).unwrap();
        assert!(friend_kid.addr.starts_with("127.0.0.1:"), "reachable for a takeover: {}", friend_kid.addr);
        assert!(matches!(next(&host), FromHub::Kids(k) if k.len() == 2));

        // The friend's edit comes back to both, in order.
        let patch = Patch::build(IRect::new(1, 1, 3, 3), |_, _| Some([9, 9, 9, 255])).unwrap();
        friend.send(ToHub::Edit { id: 1, edit: Edit::Patch(patch.clone()) });
        for link in [&host, &friend] {
            match next(link) {
                FromHub::Sequenced { seq: 1, from, id: 1, edit: Edit::Patch(p) } => {
                    assert_eq!(from, friend_id);
                    assert_eq!(p, patch);
                }
                other => panic!("{other:?}"),
            }
        }
        assert_ne!(host_id, friend_id);

        // The host leaving ends the friend's session.
        drop(host);
        for _ in 0..500 {
            match friend.try_recv() {
                Some(LinkEvent::Closed(_)) => return,
                Some(_) | None => thread::sleep(Duration::from_millis(10)),
            }
        }
        panic!("the friend never noticed the host leaving");
    }
}
