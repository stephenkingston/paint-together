//! Friendly names and colours for kids drawing together.

/// Bright colours that are easy to tell apart, for each kid's pointer and name tag.
pub const COLORS: [[u8; 3]; 12] = [
    [230, 57, 70],
    [244, 140, 6],
    [233, 196, 0],
    [42, 157, 70],
    [0, 150, 199],
    [67, 97, 238],
    [131, 56, 236],
    [222, 49, 155],
    [0, 168, 150],
    [158, 98, 50],
    [112, 193, 25],
    [255, 92, 138],
];

const ADJECTIVES: [&str; 16] = [
    "Happy", "Brave", "Sunny", "Clever", "Bouncy", "Giggly", "Sparkly", "Speedy", "Gentle", "Jolly", "Lucky", "Cosy", "Fluffy", "Zippy",
    "Wiggly", "Mighty",
];

const ANIMALS: [&str; 16] = [
    "Panda", "Tiger", "Penguin", "Fox", "Otter", "Koala", "Dolphin", "Owl", "Bunny", "Turtle", "Kitten", "Puppy", "Llama", "Hedgehog",
    "Frog", "Lion",
];

/// A fresh fun name and colour, different each time.
pub fn random() -> (String, [u8; 3]) {
    let seed = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map_or(7, |d| d.as_nanos() as u64)
        ^ (std::process::id() as u64) << 32;
    let mut x = seed | 1;
    let mut roll = |n: usize| {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        (x % n as u64) as usize
    };
    let name = format!("{} {}", ADJECTIVES[roll(ADJECTIVES.len())], ANIMALS[roll(ANIMALS.len())]);
    (name, COLORS[roll(COLORS.len())])
}

/// What a typed name becomes: trimmed, at most 20 characters, never empty.
pub fn tidy(name: &str) -> String {
    let name: String = name.trim().chars().filter(|c| !c.is_control()).take(20).collect();
    if name.is_empty() { "Friend".to_owned() } else { name }
}
