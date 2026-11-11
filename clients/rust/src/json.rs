//! Just enough JSON for Seek's replies and requests: they are small objects of strings, numbers, booleans and lists.

#[derive(Debug, Clone, PartialEq)]
pub(crate) enum Value {
    Null,
    Bool(bool),
    Num(f64),
    Str(String),
    Arr(Vec<Value>),
    Obj(Vec<(String, Value)>),
}

impl Value {
    pub(crate) fn get(&self, key: &str) -> Option<&Value> {
        match self {
            Value::Obj(o) => o.iter().find(|(k, _)| k == key).map(|(_, v)| v),
            _ => None,
        }
    }
    pub(crate) fn str(&self, key: &str) -> Option<&str> {
        match self.get(key) {
            Some(Value::Str(s)) => Some(s),
            _ => None,
        }
    }
    pub(crate) fn bool(&self, key: &str) -> Option<bool> {
        match self.get(key) {
            Some(Value::Bool(b)) => Some(*b),
            _ => None,
        }
    }
    pub(crate) fn num(&self, key: &str) -> Option<f64> {
        match self.get(key) {
            Some(Value::Num(n)) => Some(*n),
            _ => None,
        }
    }
}

pub(crate) fn parse(text: &str) -> Option<Value> {
    let mut p = Parser { s: text.as_bytes(), at: 0 };
    let v = p.value()?;
    p.ws();
    (p.at == p.s.len()).then_some(v)
}

struct Parser<'a> {
    s: &'a [u8],
    at: usize,
}

impl Parser<'_> {
    fn ws(&mut self) {
        while self.at < self.s.len() && matches!(self.s[self.at], b' ' | b'\t' | b'\n' | b'\r') {
            self.at += 1;
        }
    }
    fn eat(&mut self, w: &[u8]) -> bool {
        if self.s[self.at..].starts_with(w) {
            self.at += w.len();
            true
        } else {
            false
        }
    }
    fn value(&mut self) -> Option<Value> {
        self.ws();
        match *self.s.get(self.at)? {
            b'{' => {
                self.at += 1;
                let mut o = Vec::new();
                self.ws();
                if self.eat(b"}") {
                    return Some(Value::Obj(o));
                }
                loop {
                    self.ws();
                    let k = self.string()?;
                    self.ws();
                    if !self.eat(b":") {
                        return None;
                    }
                    let v = self.value()?;
                    o.push((k, v));
                    self.ws();
                    if self.eat(b"}") {
                        return Some(Value::Obj(o));
                    }
                    if !self.eat(b",") {
                        return None;
                    }
                }
            }
            b'[' => {
                self.at += 1;
                let mut a = Vec::new();
                self.ws();
                if self.eat(b"]") {
                    return Some(Value::Arr(a));
                }
                loop {
                    a.push(self.value()?);
                    self.ws();
                    if self.eat(b"]") {
                        return Some(Value::Arr(a));
                    }
                    if !self.eat(b",") {
                        return None;
                    }
                }
            }
            b'"' => self.string().map(Value::Str),
            b't' => self.eat(b"true").then_some(Value::Bool(true)),
            b'f' => self.eat(b"false").then_some(Value::Bool(false)),
            b'n' => self.eat(b"null").then_some(Value::Null),
            _ => {
                let start = self.at;
                while self.at < self.s.len() && matches!(self.s[self.at], b'-' | b'+' | b'.' | b'e' | b'E' | b'0'..=b'9') {
                    self.at += 1;
                }
                std::str::from_utf8(&self.s[start..self.at]).ok()?.parse().ok().map(Value::Num)
            }
        }
    }
    fn hex4(&mut self) -> Option<u32> {
        let h = std::str::from_utf8(self.s.get(self.at..self.at + 4)?).ok()?;
        self.at += 4;
        u32::from_str_radix(h, 16).ok()
    }
    fn string(&mut self) -> Option<String> {
        if !self.eat(b"\"") {
            return None;
        }
        let mut out = Vec::new();
        loop {
            let c = *self.s.get(self.at)?;
            self.at += 1;
            match c {
                b'"' => return String::from_utf8(out).ok(),
                b'\\' => {
                    let e = *self.s.get(self.at)?;
                    self.at += 1;
                    let ch = match e {
                        b'n' => '\n',
                        b't' => '\t',
                        b'r' => '\r',
                        b'b' => '\u{8}',
                        b'f' => '\u{c}',
                        b'u' => {
                            let mut cp = self.hex4()?;
                            if (0xD800..0xDC00).contains(&cp) && self.eat(b"\\u") {
                                let lo = self.hex4()?;
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo.wrapping_sub(0xDC00) & 0x3FF);
                            }
                            char::from_u32(cp).unwrap_or('\u{FFFD}')
                        }
                        other => other as char,
                    };
                    let mut b = [0u8; 4];
                    out.extend_from_slice(ch.encode_utf8(&mut b).as_bytes());
                }
                _ => out.push(c),
            }
        }
    }
}

/// Appends a JSON string, quotes included.
pub(crate) fn quote(s: &str, out: &mut String) {
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reads_a_reply() {
        let v = parse(r##"{"api":1,"version":"0.5.0","text":"#rows\t1\nF\tC:\\a\\b.txt\n","error":false,"apis":[1],"x":null,"e":"é😀"}"##).unwrap();
        assert_eq!(v.num("api"), Some(1.0));
        assert_eq!(v.str("text"), Some("#rows\t1\nF\tC:\\a\\b.txt\n"));
        assert_eq!(v.bool("error"), Some(false));
        assert_eq!(v.str("e"), Some("é😀"));
        assert_eq!(v.get("apis"), Some(&Value::Arr(vec![Value::Num(1.0)])));
    }

    #[test]
    fn quotes() {
        let mut s = String::new();
        quote("a\"b\\c\nd\u{1}", &mut s);
        assert_eq!(s, r#""a\"b\\c\nd\u0001""#);
        assert_eq!(parse(&s), Some(Value::Str("a\"b\\c\nd\u{1}".into())));
    }

    #[test]
    fn says_no_to_broken_text() {
        assert_eq!(parse("{\"a\":"), None);
        assert_eq!(parse("{\"a\":1} x"), None);
    }
}
