//! Finding drawings on the same network with mDNS (the way printers are found),
//! so kids can tap a friend's drawing instead of typing an address.

use std::net::{IpAddr, SocketAddr};

use mdns_sd::{ServiceDaemon, ServiceEvent, ServiceInfo};

use super::protocol::VERSION;

const SERVICE: &str = "_paintparty._tcp.local.";

/// A drawing someone nearby is hosting.
#[derive(Clone, Debug, PartialEq)]
pub struct Nearby {
    fullname: String,
    /// The host kid's name.
    pub name: String,
    pub kids: usize,
    pub addr: SocketAddr,
    /// Made by the same version of Paint (others can't be joined).
    pub compatible: bool,
}

pub struct Discovery {
    daemon: ServiceDaemon,
    browse: Option<mdns_sd::Receiver<ServiceEvent>>,
    pub nearby: Vec<Nearby>,
    /// My own drawing's name on the network, when hosting.
    advertised: Option<String>,
    /// A name for this app on the network, unique enough not to clash.
    instance: String,
}

impl Discovery {
    pub fn new() -> Option<Self> {
        let daemon = ServiceDaemon::new().ok()?;
        let seed = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map_or(0, |d| d.as_nanos() as u64);
        let instance = format!("paint-{:08x}", (seed ^ (std::process::id() as u64) << 20) as u32);
        Some(Self { daemon, browse: None, nearby: Vec::new(), advertised: None, instance })
    }

    /// Start looking for drawings (once).
    pub fn browse(&mut self) {
        if self.browse.is_none() {
            self.browse = self.daemon.browse(SERVICE).ok();
        }
    }

    /// Take in what the network has said; true if the list changed.
    pub fn poll(&mut self) -> bool {
        let Some(rx) = &self.browse else { return false };
        let mut changed = false;
        while let Ok(event) = rx.try_recv() {
            match event {
                ServiceEvent::ServiceResolved(info) => {
                    if Some(&info.fullname) == self.advertised.as_ref() {
                        continue;
                    }
                    let Some(ip) = info.get_addresses_v4().into_iter().min() else {
                        continue;
                    };
                    let found = Nearby {
                        fullname: info.fullname.clone(),
                        name: info.get_property_val_str("name").unwrap_or("A friend").chars().take(24).collect(),
                        kids: info.get_property_val_str("kids").and_then(|k| k.parse().ok()).unwrap_or(1),
                        addr: SocketAddr::new(IpAddr::V4(ip), info.get_port()),
                        compatible: info.get_property_val_str("v") == Some(&VERSION.to_string()),
                    };
                    match self.nearby.iter_mut().find(|n| n.fullname == found.fullname) {
                        Some(n) if *n == found => {}
                        Some(n) => {
                            *n = found;
                            changed = true;
                        }
                        None => {
                            self.nearby.push(found);
                            changed = true;
                        }
                    }
                }
                ServiceEvent::ServiceRemoved(_, fullname) => {
                    let before = self.nearby.len();
                    self.nearby.retain(|n| n.fullname != fullname);
                    changed |= self.nearby.len() != before;
                }
                _ => {}
            }
        }
        changed
    }

    /// Tell the network about my drawing (again, when the number of kids changes).
    pub fn advertise(&mut self, name: &str, kids: usize, port: u16) {
        let host = format!("{}.local.", self.instance);
        let props = [("name", name.to_owned()), ("kids", kids.to_string()), ("v", VERSION.to_string())];
        let Ok(info) = ServiceInfo::new(SERVICE, &self.instance, &host, (), port, &props[..]) else {
            return;
        };
        let info = info.enable_addr_auto();
        self.advertised = Some(info.get_fullname().to_owned());
        self.nearby.retain(|n| Some(&n.fullname) != self.advertised.as_ref());
        let _ = self.daemon.register(info);
    }

    pub fn stop_advertising(&mut self) {
        if let Some(fullname) = self.advertised.take() {
            let _ = self.daemon.unregister(&fullname);
        }
    }
}

impl Drop for Discovery {
    fn drop(&mut self) {
        self.stop_advertising();
        let _ = self.daemon.shutdown();
    }
}
