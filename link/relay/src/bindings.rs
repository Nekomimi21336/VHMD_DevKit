//! Button bindings: which SteamVR input each headset button presses, on the
//! same hand. The PC app sends them as lines "left.menu=system" (BINDINGS on
//! the control socket); Python's data path does the same (bindings.py).
//!
//! Sources are the names the headset sends. The left menu button is "menu" or,
//! from newer headset apps, "system": one physical button. A target that is
//! not a source itself stays pressable by its own button. "none" presses nothing.

use std::borrow::Cow;

use vhmd_link_proto::*;

const LEFT: [&str; 8] = ["system", "menu", "x", "y", "joystick_click", "trigger_click", "grip_click", "none"];
const RIGHT: [&str; 8] = ["system", "menu", "a", "b", "joystick_click", "trigger_click", "grip_click", "none"];

#[derive(Default, Clone, PartialEq, Debug)]
struct Hand {
    map: Vec<(String, String)>, // source -> target
    emit: Vec<String>,          // every name this hand's message then carries
}

#[derive(Default, Clone, PartialEq, Debug)]
pub struct Bindings {
    hands: [Hand; 2], // left, right
}

impl Hand {
    fn add(&mut self, src: &str, dst: &str) {
        match self.map.iter_mut().find(|(s, _)| s == src) {
            Some(e) => e.1 = dst.to_string(),
            None => self.map.push((src.to_string(), dst.to_string())),
        }
    }

    fn finish(&mut self) {
        let targets: Vec<String> = self.map.iter().map(|(_, d)| d.clone()).collect();
        for d in &targets {
            let src = if d == "system" { "menu" } else { d.as_str() };
            if d != "none" && !self.map.iter().any(|(s, _)| s == src) {
                self.map.push((src.to_string(), d.clone()));
            }
        }
        for (s, d) in &self.map {
            for name in [s.as_str(), d.as_str()] {
                if name != "none" && !self.emit.iter().any(|e| e == name) {
                    self.emit.push(name.to_string());
                }
            }
        }
        if self.map.iter().any(|(s, _)| s == "menu") && !self.emit.iter().any(|e| e == "system") {
            self.emit.push("system".to_string()); // what a newer headset app sends for it
        }
    }
}

impl Bindings {
    /// From lines "left.menu=system"; anything else is skipped.
    pub fn parse(text: &str) -> Bindings {
        let mut b = Bindings::default();
        for line in text.lines() {
            let Some((name, dst)) = line.trim().split_once('=') else { continue };
            let Some((hand, src)) = name.split_once('.') else { continue };
            let (i, targets) = match hand {
                "left" => (0, &LEFT),
                "right" => (1, &RIGHT),
                _ => continue,
            };
            if targets.contains(&dst) && targets.contains(&src) && src != "none" {
                b.hands[i].add(if src == "system" { "menu" } else { src }, dst);
            }
        }
        b.hands.iter_mut().for_each(Hand::finish);
        b
    }

    /// The messages of one JSON array with the buttons moved.
    pub fn apply<'a>(&self, text: &'a str) -> Cow<'a, str> {
        if self.hands.iter().all(|h| h.map.is_empty()) || !text.contains("\"type\":\"input\"") {
            return text.into();
        }
        let mut out = String::with_capacity(text.len() + 64);
        out.push('[');
        for obj in array_objects(text) {
            if out.len() > 1 {
                out.push(',');
            }
            if !self.rewrite(obj, &mut out) {
                out.push_str(obj);
            }
        }
        out.push(']');
        out.into()
    }

    /// One input message of a hand, into out; false to keep it as it is.
    fn rewrite(&self, obj: &str, out: &mut String) -> bool {
        if message_type(obj).as_deref() != Some("input") {
            return false;
        }
        let hand = match json_string(obj, "id").as_deref() {
            Some("left_hand") => &self.hands[0],
            Some("right_hand") => &self.hands[1],
            _ => return false,
        };
        if hand.map.is_empty() {
            return false;
        }
        let Some(key) = obj.find("\"values\"") else { return false };
        let Some(open) = obj[key..].find('{').map(|i| key + i) else { return false };
        let Some(close) = obj[open..].find('}').map(|i| open + i) else { return false };
        let body = &obj[open + 1..close];

        let mut pressed: Vec<&str> = Vec::with_capacity(4);
        let start = out.len();
        out.push_str(&obj[..=open]);
        let first = out.len();
        for pair in body.split(',') {
            let Some((k, v)) = pair.split_once(':') else {
                out.truncate(start);
                return false;
            };
            let k = k.trim().trim_matches('"');
            let src = if k == "system" { "menu" } else { k };
            if let Some((_, dst)) = hand.map.iter().find(|(s, _)| s == src) {
                if v.trim() == "true" {
                    pressed.push(dst);
                }
                continue;
            }
            if out.len() > first {
                out.push(',');
            }
            out.push_str(pair);
        }
        for name in &hand.emit {
            if out.len() > first {
                out.push(',');
            }
            out.push('"');
            out.push_str(name);
            out.push_str(if pressed.contains(&name.as_str()) { "\":true" } else { "\":false" });
        }
        out.push_str(&obj[close..]);
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const HEAD: &str = r#"{"type":"pose","id":"hmd","pos":[0,1.6,0],"src":"manual"}"#;

    fn input(hand: &str, values: &str) -> String {
        format!(r#"{{"type":"input","id":"{hand}_hand","src":"manual","values":{{"trigger":0.250,{values}}}}}"#)
    }

    fn apply(b: &str, msg: &str) -> String {
        let text = format!("[{HEAD},{msg}]");
        Bindings::parse(b).apply(&text).into_owned()
    }

    #[test]
    fn menu_to_system() {
        // An older headset app sends "menu", a newer one "system": the same button.
        for key in ["menu", "system"] {
            let got = apply("left.menu=system", &input("left", &format!(r#""x":false,"{key}":true"#)));
            assert!(got.starts_with(&format!("[{HEAD},")));
            assert!(got.contains(r#""trigger":0.250,"x":false,"#), "{got}");
            assert!(got.contains(r#""menu":false"#) && got.contains(r#""system":true"#), "{got}");
        }
        let got = apply("left.menu=system", &input("left", r#""menu":false"#));
        assert!(got.contains(r#""system":false"#), "{got}");
    }

    #[test]
    fn newer_app_back_to_menu() {
        let got = apply("left.menu=menu", &input("left", r#""system":true"#));
        assert!(got.contains(r#""menu":true"#) && got.contains(r#""system":false"#), "{got}");
    }

    #[test]
    fn swap_merge_none() {
        let got = apply("right.a=b\nright.b=a", &input("right", r#""a":true,"b":false"#));
        assert!(got.contains(r#""b":true"#) && got.contains(r#""a":false"#), "{got}");
        // Two buttons on one input: either presses it.
        let got = apply("right.a=b", &input("right", r#""a":false,"b":true"#));
        assert!(got.contains(r#""b":true"#) && got.contains(r#""a":false"#), "{got}");
        let got = apply("left.x=none", &input("left", r#""x":true"#));
        assert!(got.contains(r#""x":false"#), "{got}");
    }

    #[test]
    fn other_hand_and_messages_unchanged() {
        let msg = input("right", r#""a":true"#);
        assert_eq!(apply("left.menu=system", &msg), format!("[{HEAD},{msg}]"));
        let text = format!("[{HEAD}]");
        assert!(matches!(Bindings::parse("left.menu=system").apply(&text), Cow::Borrowed(_)));
        let msg = input("left", r#""menu":true"#);
        assert!(matches!(Bindings::parse("").apply(&format!("[{msg}]")), Cow::Borrowed(_)));
    }

    #[test]
    fn bad_lines_skipped() {
        let b = Bindings::parse("left.menu\nmiddle.a=b\nleft.a=system\nleft.x=trigger\nleft.none=x");
        assert_eq!(b, Bindings::default());
    }

    #[test]
    fn speed() {
        let b = Bindings::parse("left.menu=system\nright.a=b\nright.b=a");
        let left = input("left", r#""trigger_click":false,"grip":0.000,"joystick_x":0.000,"joystick_y":0.000,"x":false,"y":false,"menu":true"#);
        let right = input("right", r#""trigger_click":false,"grip":0.000,"joystick_x":0.000,"joystick_y":0.000,"a":false,"b":true"#);
        let text = format!("[{HEAD},{HEAD},{HEAD},{left},{right}]");
        let t = std::time::Instant::now();
        let mut n = 0;
        for _ in 0..10000 {
            n += b.apply(&text).len();
        }
        let us = t.elapsed().as_secs_f64() * 1e6 / 10000.0;
        println!("bindings: {us:.2} us per message ({n})");
        assert!(us < 500.0);
    }
}
