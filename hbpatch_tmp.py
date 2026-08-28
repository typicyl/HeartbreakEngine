import io
def patch(path, pairs):
    raw = io.open(path, "rb").read()
    nl = "\r\n" if b"\r\n" in raw else "\n"
    s = raw.decode("utf-8")
    for tag, old, new in pairs:
        o = old.replace("\n", nl); n = new.replace("\n", nl)
        if s.count(o) != 1:
            if s.count(old) == 1: o, n = old, new
            else: raise AssertionError((path, tag, s.count(o), s.count(old)))
        s = s.replace(o, n)
    io.open(path, "w", encoding="utf-8", newline="").write(s)
    print("patched", path)
