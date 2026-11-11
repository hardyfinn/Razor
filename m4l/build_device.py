#!/usr/bin/env python3
"""Builds Razor.amxd, the Max for Live audio effect version of Razor.

    python3 m4l/build_device.py            writes m4l/dist/Razor.amxd (+ .maxpat)

Patch overview:

  plugin~ -> gen~ (m4l/razor.genexpr in a codebox) -> plugout~
  phasor~ 4n @lock 1 (beat) and phasor~ 1n @lock 1 (bar) -> gen~ inlets 3, 4
      so the tape follows Live's transport and tempo
  transport -> tempo -> gen~ (used before the first beat)
  the four reverse buttons -> "armlen <i>" then counter -> "armseq <n>" -> gen~
      (they're Live parameters, so Key Map, MIDI Map and automation all work)
  dials / toggles / menu -> "<param> <value>" -> gen~
  gen~ writes peaks and state into buffer~ ---razorpk / ---razorst
  jsui (razor_display.js) reads those buffers 25 times a second
"""
import json, os, struct

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "dist")


class Patch:
    def __init__(self):
        self.boxes, self.lines, self.n = [], [], 0

    def add(self, box):
        self.n += 1
        box["id"] = "obj-%d" % self.n
        self.boxes.append({"box": box})
        return box["id"]

    def obj(self, text, x, y, ins=1, outs=1, types=None, w=None, **extra):
        b = {"maxclass": "newobj", "text": text, "numinlets": ins, "numoutlets": outs,
             "outlettype": types if types is not None else [""] * outs,
             "patching_rect": [x, y, w or max(30, 7 * len(text) + 12), 22.0]}
        b.update(extra)
        return self.add(b)

    def msg(self, text, x, y):
        return self.add({"maxclass": "message", "text": text, "numinlets": 2, "numoutlets": 1,
                         "outlettype": [""], "patching_rect": [x, y, max(30, 7 * len(text) + 14), 22.0]})

    def comment(self, text, x, y, w=300, pres=None, size=None, color=None):
        b = {"maxclass": "comment", "text": text, "numinlets": 1, "numoutlets": 0,
             "patching_rect": [x, y, w, 20.0]}
        if pres:
            b.update({"presentation": 1, "presentation_rect": pres,
                      "textcolor": color or [0.75, 0.75, 0.75, 1.0]})
        if size:
            b["fontsize"] = size
        return self.add(b)

    def connect(self, a, ao, b, bi=0):
        self.lines.append({"patchline": {"source": [a, ao], "destination": [b, bi]}})


def valueof(longname, short, ptype, extra):
    v = {"parameter_longname": longname, "parameter_shortname": short, "parameter_type": ptype}
    v.update(extra)
    return {"valueof": v}


def patcher_dict(boxes, lines, rect, classnamespace="box", extra=None):
    d = {"fileversion": 1,
         "appversion": {"major": 9, "minor": 0, "revision": 0, "architecture": "x64", "modernui": 1},
         "classnamespace": classnamespace, "rect": rect, "bglocked": 0, "openinpresentation": 1,
         "default_fontsize": 10.0, "default_fontface": 0, "default_fontname": "Arial Bold",
         "gridonopen": 1, "gridsize": [15.0, 15.0], "gridsnaponopen": 1, "objectsnaponopen": 1,
         "statusbarvisible": 2, "toolbarvisible": 1, "lefttoolbarpinned": 0, "toptoolbarpinned": 0,
         "righttoolbarpinned": 0, "bottomtoolbarpinned": 0, "toolbars_unpinned_last_save": 0,
         "tallnewobj": 0, "boxanimatetime": 200, "enablehscroll": 1, "enablevscroll": 1,
         "devicewidth": 0.0, "description": "", "digest": "", "tags": "", "style": "",
         "subpatcher_template": "", "assistshowspatchername": 0,
         "boxes": boxes, "lines": lines}
    if extra:
        d.update(extra)
    return d


def gen_box(P, code, x, y):
    """A gen~ object with the engine in a codebox, 4 inlets and 3 outlets."""
    G = Patch()
    cb = G.add({"maxclass": "codebox", "code": code, "fontface": 0, "fontname": "<Monospaced>",
                "fontsize": 12.0, "numinlets": 4, "numoutlets": 3, "outlettype": ["", "", ""],
                "patching_rect": [50.0, 100.0, 700.0, 600.0]})
    for i in range(4):
        iid = G.add({"maxclass": "newobj", "text": "in %d" % (i + 1), "numinlets": 0, "numoutlets": 1,
                     "outlettype": [""], "patching_rect": [50.0 + 180 * i, 40.0, 40.0, 22.0]})
        G.connect(iid, 0, cb, i)
    for i in range(3):
        oid = G.add({"maxclass": "newobj", "text": "out %d" % (i + 1), "numinlets": 1, "numoutlets": 0,
                     "patching_rect": [50.0 + 250 * i, 730.0, 45.0, 22.0]})
        G.connect(cb, i, oid, 0)
    gp = patcher_dict(G.boxes, G.lines, [100.0, 100.0, 820.0, 800.0], classnamespace="dsp.gen",
                      extra={"openinpresentation": 0})
    return P.obj("gen~", x, y, 4, 3, ["signal", "signal", "signal"], w=120, patcher=gp)


def dial(P, long, short, lo, hi, dflt, unitstyle, x, y, pres, units=None, exponent=None):
    extra = {"parameter_mmin": lo, "parameter_mmax": hi, "parameter_initial_enable": 1,
             "parameter_initial": [dflt], "parameter_unitstyle": unitstyle}
    if units:
        extra["parameter_units"] = units
    return P.add({"maxclass": "live.dial", "varname": long, "numinlets": 1, "numoutlets": 2,
                  "outlettype": ["", "float"], "patching_rect": [x, y, 44, 48],
                  "presentation": 1, "presentation_rect": pres, "parameter_enable": 1,
                  "saved_attribute_attributes": valueof(long, short, 0, extra)})


def toggle(P, long, short, text, x, y, pres, dflt=0):
    extra = {"parameter_enum": ["off", "on"], "parameter_mmax": 1, "parameter_initial_enable": 1,
             "parameter_initial": [dflt]}
    return P.add({"maxclass": "live.text", "varname": long, "text": text, "texton": text, "mode": 1,
                  "numinlets": 1, "numoutlets": 2, "outlettype": ["", ""], "patching_rect": [x, y, 44, 15],
                  "presentation": 1, "presentation_rect": pres, "parameter_enable": 1,
                  "saved_attribute_attributes": valueof(long, short, 2, extra)})


def button(P, long, short, text, x, y, pres, fontsize=None):
    b = {"maxclass": "live.text", "varname": long, "text": text, "texton": text, "mode": 0,
         "numinlets": 1, "numoutlets": 2, "outlettype": ["", ""], "patching_rect": [x, y, 44, 15],
         "presentation": 1, "presentation_rect": pres, "parameter_enable": 1,
         "saved_attribute_attributes": valueof(long, short, 2, {"parameter_enum": ["off", "on"],
                                                                "parameter_mmax": 1})}
    if fontsize:
        b["fontsize"] = fontsize
    return P.add(b)


def build():
    code = open(os.path.join(HERE, "razor.genexpr")).read()
    P = Patch()

    # ---- audio ----
    P.comment("Razor: live reverse-tape splice. Engine: the gen~ codebox (m4l/razor.genexpr).", 20, 10, 520)
    pin = P.obj("plugin~", 20, 50, 1, 2, ["signal", "signal"])
    beat = P.obj("phasor~ 4n @lock 1", 160, 50, 2, 1, ["signal"])
    bar = P.obj("phasor~ 1n @lock 1", 320, 50, 2, 1, ["signal"])
    gen = gen_box(P, code, 20, 330)
    P.connect(pin, 0, gen, 0)
    P.connect(pin, 1, gen, 1)
    P.connect(beat, 0, gen, 2)
    P.connect(bar, 0, gen, 3)
    pout = P.obj("plugout~", 20, 380, 2, 2, ["signal", "signal"])
    P.connect(gen, 0, pout, 0)
    P.connect(gen, 1, pout, 1)

    # ---- tempo from Live (used until the first beat arrives) ----
    tm = P.obj("qmetro 200 @active 1", 480, 50, 2, 1, ["bang"])
    tr = P.obj("transport", 480, 85, 2, 9, ["int", "int", "float", "float", "float", "", "int", "float", ""])
    P.connect(tm, 0, tr, 0)
    tch = P.obj("change 0.", 480, 120, 1, 3, ["", "int", "int"])
    P.connect(tr, 4, tch, 0)
    tpre = P.obj("prepend tempo", 480, 155)
    P.connect(tch, 0, tpre, 0)
    P.connect(tpre, 0, gen, 0)

    # ---- buffers for the display ----
    dev = P.obj("live.thisdevice", 680, 50, 1, 3, ["bang", "int", "int"])
    tb = P.obj("t b b b", 680, 85, 1, 3, ["bang", "bang", "bang"])
    P.connect(dev, 0, tb, 0)
    bpk = P.obj("buffer~ ---razorpk", 680, 190, 1, 2, ["float", "bang"], w=140)
    bst = P.obj("buffer~ ---razorst", 840, 190, 1, 2, ["float", "bang"], w=140)
    m1 = P.msg("sizeinsamps 8192 1", 680, 120)
    m2 = P.msg("sizeinsamps 32 1", 840, 120)
    P.connect(tb, 2, m1, 0)
    P.connect(tb, 2, m2, 0)
    P.connect(m1, 0, bpk, 0)
    P.connect(m2, 0, bst, 0)
    mbind = P.msg("pk ---razorpk, st ---razorst", 680, 240)
    P.connect(tb, 1, mbind, 0)
    P.connect(mbind, 0, gen, 0)

    # ---- display ----
    disp = P.add({"maxclass": "jsui", "filename": "razor_display.js", "numinlets": 1, "numoutlets": 1,
                  "outlettype": [""], "parameter_enable": 0, "border": 0,
                  "patching_rect": [680, 330, 300, 92], "presentation": 1, "presentation_rect": [4, 4, 300, 96]})
    mbufs = P.msg("bufs ---razorpk ---razorst", 680, 280)
    P.connect(tb, 0, mbufs, 0)
    P.connect(mbufs, 0, disp, 0)
    dm = P.obj("qmetro 40 @active 1", 1000, 280, 2, 1, ["bang"])
    P.connect(dm, 0, disp, 0)

    # ---- the four reverse buttons ----
    # button -> route bang 1 -> t b b: first "armlen i", then the counter bumps armseq
    cnt = P.obj("counter 0 1 1000000", 20, 560, 3, 4, ["int", "", "", "int"])
    cpre = P.obj("prepend armseq", 20, 595)
    P.connect(cnt, 0, cpre, 0)
    P.connect(cpre, 0, gen, 0)
    labels = ["½", "1", "2", "4"]
    longs = ["Reverse 1/2 beat", "Reverse 1 beat", "Reverse 2 beats", "Reverse 4 beats"]
    shorts = ["Rev 1/2", "Rev 1", "Rev 2", "Rev 4"]
    for i in range(4):
        x = 20 + 150 * i
        bid = button(P, longs[i], shorts[i], labels[i], x, 440, [4 + 75 * i, 106, 71, 40], fontsize=18.0)
        rt = P.obj("route bang 1", x, 470, 1, 3)
        P.connect(bid, 0, rt, 0)
        t = P.obj("t b b", x, 500, 1, 2, ["bang", "bang"])
        P.connect(rt, 0, t, 0)
        P.connect(rt, 1, t, 0)
        m = P.msg("armlen %d" % i, x + 50, 530)
        P.connect(t, 1, m, 0)
        P.connect(m, 0, gen, 0)
        P.connect(t, 0, cnt, 0)
    P.comment("reverse the next ... beats   (Key Map: Cmd/Ctrl+K, MIDI Map: Cmd/Ctrl+M)", 0, 0, 10,
              pres=[4, 148, 300, 16], size=9.0)

    # ---- dials ----
    cut = dial(P, "Cut", "Cut", -100.0, 100.0, 0.0, 5, 640, 440, [314, 4, 44, 48])
    cdiv = P.obj("/ 100.", 640, 500)
    cpr = P.obj("prepend cut", 640, 530)
    P.connect(cut, 0, cdiv, 0)
    P.connect(cdiv, 0, cpr, 0)
    P.connect(cpr, 0, gen, 0)
    ang = dial(P, "Splice Angle", "Angle", 22.5, 90.0, 45.0, 9, 740, 440, [364, 4, 44, 48], units="%.1f°")
    apr = P.obj("prepend angle", 740, 530)
    P.connect(ang, 0, apr, 0)
    P.connect(apr, 0, gen, 0)

    # ---- toggles ----
    for j, (long, short, text, name) in enumerate([("Snap", "Snap", "SNAP", "snap"),
                                                   ("Start on Bar", "Bar", "BAR", "barq"),
                                                   ("Insert", "Insert", "INSERT", "insert")]):
        x = 840 + 90 * j
        tid = toggle(P, long, short, text, x, 440, [314 + 32 * j, 58, 30, 18])
        pr = P.obj("prepend " + name, x, 530)
        P.connect(tid, 0, pr, 0)
        P.connect(pr, 0, gen, 0)
    # the INSERT toggle is wider, so lay the three out by hand
    for b in P.boxes:
        bx = b["box"]
        if bx.get("varname") == "Snap":
            bx["presentation_rect"] = [314, 58, 30, 18]
        if bx.get("varname") == "Start on Bar":
            bx["presentation_rect"] = [346, 58, 26, 18]
        if bx.get("varname") == "Insert":
            bx["presentation_rect"] = [374, 58, 38, 18]

    # ---- catch-up speed ----
    menu = P.add({"maxclass": "live.menu", "varname": "Catch-up", "numinlets": 1, "numoutlets": 3,
                  "outlettype": ["", "", "float"], "patching_rect": [1120, 440, 100, 15],
                  "presentation": 1, "presentation_rect": [314, 96, 98, 15], "parameter_enable": 1,
                  "saved_attribute_attributes": valueof("Catch-up", "Catch-up", 2, {
                      "parameter_enum": ["Gentle 2%", "Medium 6%", "Fast 12%"], "parameter_mmax": 2,
                      "parameter_initial_enable": 1, "parameter_initial": [1]})})
    P.comment("insert catch-up", 0, 0, 10, pres=[314, 80, 98, 16], size=9.0)
    sel = P.obj("sel 0 1 2", 1120, 470, 1, 4)
    P.connect(menu, 0, sel, 0)
    for i, rate in enumerate(["0.02", "0.06", "0.12"]):
        m = P.msg("catchrate " + rate, 1120 + 100 * i, 500)
        P.connect(sel, i, m, 0)
        P.connect(m, 0, gen, 0)

    # ---- clear ----
    clr = P.add({"maxclass": "live.text", "varname": "Clear tape", "text": "Clear tape", "mode": 0,
                 "numinlets": 1, "numoutlets": 2, "outlettype": ["", ""], "patching_rect": [1120, 560, 60, 15],
                 "presentation": 1, "presentation_rect": [314, 120, 98, 18], "parameter_enable": 1,
                 "saved_attribute_attributes": valueof("Clear tape", "Clear", 2, {
                     "parameter_enum": ["off", "on"], "parameter_mmax": 1})})
    crt = P.obj("route bang 1", 1120, 590, 1, 3)
    P.connect(clr, 0, crt, 0)
    ccnt = P.obj("counter 0 1 1000000", 1120, 620, 3, 4, ["int", "", "", "int"])
    P.connect(crt, 0, ccnt, 0)
    P.connect(crt, 1, ccnt, 0)
    cpre2 = P.obj("prepend clearseq", 1120, 650)
    P.connect(ccnt, 0, cpre2, 0)
    P.connect(cpre2, 0, gen, 0)

    # ---- REV light: gen~ out 3 ----
    snap = P.obj("snapshot~ 30", 300, 380, 2, 1, ["float"])
    P.connect(gen, 2, snap, 0)
    led = P.add({"maxclass": "live.text", "varname": "rev_led", "text": "REV", "texton": "REV", "mode": 1,
                 "numinlets": 1, "numoutlets": 2, "outlettype": ["", ""], "patching_rect": [300, 420, 44, 15],
                 "presentation": 1, "presentation_rect": [314, 144, 98, 18], "parameter_enable": 0,
                 "ignoreclick": 1, "activebgoncolor": [0.94, 0.48, 0.23, 1.0]})
    chg = P.obj("change", 300, 400, 1, 3, ["", "int", "int"])
    P.connect(snap, 0, chg, 0)
    P.connect(chg, 0, led, 0)

    longs_all = [b["box"]["saved_attribute_attributes"]["valueof"]["parameter_longname"]
                 for b in P.boxes if "saved_attribute_attributes" in b["box"]]
    assert len(set(longs_all)) == len(longs_all), "duplicate parameter names"

    return patcher_dict(P.boxes, P.lines, [60.0, 80.0, 1300.0, 760.0],
                        extra={"devicewidth": 418.0,
                               "dependency_cache": [{"name": "razor_display.js", "bootpath": ".",
                                                     "patcherrelativepath": ".", "type": "TEXT", "implicit": 1}],
                               "project": {
                                   "version": 1, "creationdate": 3814000000, "modificationdate": 3814000000,
                                   "viewrect": [0.0, 0.0, 300.0, 500.0], "autoorganize": 1, "hideprojectwindow": 1,
                                   "showdependencies": 1, "autolocalize": 0,
                                   "contents": {"patchers": {},
                                                "code": {"razor_display.js": {"kind": "javascript", "local": 1}}},
                                   "layout": {}, "searchpath": {}, "detailsvisible": 0,
                                   "amxdtype": 1633771873,          # 'aaaa': an audio effect
                                   "readonly": 0, "devpathtype": 0, "devpath": ".", "sortmode": 0,
                                   "viewmode": 0, "includepackages": 0},
                               "latency": 0, "is_mpe": 0, "minimum_live_version": "",
                               "minimum_max_version": "", "platform_compatibility": 0, "autosave": 0})


def amxd_bytes(patcher):
    body = json.dumps({"patcher": patcher}, indent=1, ensure_ascii=False).encode("utf-8") + b"\n\x00"
    out = b"ampf" + struct.pack("<I", 4) + b"aaaa"   # aaaa = audio effect
    out += b"meta" + struct.pack("<I", 4) + b"\x00\x00\x00\x00"
    out += b"ptch" + struct.pack("<I", len(body)) + body
    return out


def main():
    os.makedirs(OUT, exist_ok=True)
    patcher = build()
    with open(os.path.join(OUT, "Razor.amxd"), "wb") as f:
        f.write(amxd_bytes(patcher))
    with open(os.path.join(OUT, "Razor.maxpat"), "w") as f:
        json.dump({"patcher": patcher}, f, indent=1, ensure_ascii=False)
    with open(os.path.join(OUT, "razor_display.js"), "w") as f:
        f.write(open(os.path.join(HERE, "razor_display.js")).read())
    print("built m4l/dist/Razor.amxd")


if __name__ == "__main__":
    main()
