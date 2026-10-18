#include "visuals/device_visuals.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>

namespace vhmd {
namespace {

// --- 5x7 pixel font (uppercase, digits, a few symbols) ----------------------
const std::map<char, std::array<const char*, 7>>& Font() {
    static const std::map<char, std::array<const char*, 7>> f = {
        {'A', {" ### ", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"}},
        {'B', {"#### ", "#   #", "#   #", "#### ", "#   #", "#   #", "#### "}},
        {'C', {" ### ", "#   #", "#    ", "#    ", "#    ", "#   #", " ### "}},
        {'D', {"#### ", "#   #", "#   #", "#   #", "#   #", "#   #", "#### "}},
        {'E', {"#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#####"}},
        {'F', {"#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#    "}},
        {'G', {" ### ", "#   #", "#    ", "# ###", "#   #", "#   #", " ####"}},
        {'H', {"#   #", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"}},
        {'I', {" ### ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### "}},
        {'J', {"  ###", "   # ", "   # ", "   # ", "   # ", "#  # ", " ##  "}},
        {'K', {"#   #", "#  # ", "# #  ", "##   ", "# #  ", "#  # ", "#   #"}},
        {'L', {"#    ", "#    ", "#    ", "#    ", "#    ", "#    ", "#####"}},
        {'M', {"#   #", "## ##", "# # #", "# # #", "#   #", "#   #", "#   #"}},
        {'N', {"#   #", "##  #", "# # #", "#  ##", "#   #", "#   #", "#   #"}},
        {'O', {" ### ", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "}},
        {'P', {"#### ", "#   #", "#   #", "#### ", "#    ", "#    ", "#    "}},
        {'Q', {" ### ", "#   #", "#   #", "#   #", "# # #", "#  # ", " ## #"}},
        {'R', {"#### ", "#   #", "#   #", "#### ", "# #  ", "#  # ", "#   #"}},
        {'S', {" ####", "#    ", "#    ", " ### ", "    #", "    #", "#### "}},
        {'T', {"#####", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  "}},
        {'U', {"#   #", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "}},
        {'V', {"#   #", "#   #", "#   #", "#   #", "#   #", " # # ", "  #  "}},
        {'W', {"#   #", "#   #", "#   #", "# # #", "# # #", "## ##", "#   #"}},
        {'X', {"#   #", "#   #", " # # ", "  #  ", " # # ", "#   #", "#   #"}},
        {'Y', {"#   #", "#   #", " # # ", "  #  ", "  #  ", "  #  ", "  #  "}},
        {'Z', {"#####", "    #", "   # ", "  #  ", " #   ", "#    ", "#####"}},
        {'0', {" ### ", "#   #", "#  ##", "# # #", "##  #", "#   #", " ### "}},
        {'1', {"  #  ", " ##  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### "}},
        {'2', {" ### ", "#   #", "    #", "   # ", "  #  ", " #   ", "#####"}},
        {'3', {"#####", "   # ", "  #  ", "   # ", "    #", "#   #", " ### "}},
        {'4', {"   # ", "  ## ", " # # ", "#  # ", "#####", "   # ", "   # "}},
        {'5', {"#####", "#    ", "#### ", "    #", "    #", "#   #", " ### "}},
        {'6', {"  ## ", " #   ", "#    ", "#### ", "#   #", "#   #", " ### "}},
        {'7', {"#####", "    #", "   # ", "  #  ", " #   ", " #   ", " #   "}},
        {'8', {" ### ", "#   #", "#   #", " ### ", "#   #", "#   #", " ### "}},
        {'9', {" ### ", "#   #", "#   #", " ####", "    #", "   # ", " ##  "}},
        {'_', {"     ", "     ", "     ", "     ", "     ", "     ", "#####"}},
        {'-', {"     ", "     ", "     ", "#####", "     ", "     ", "     "}},
        {'.', {"     ", "     ", "     ", "     ", "     ", " ##  ", " ##  "}},
        {'#', {" # # ", " # # ", "#####", " # # ", "#####", " # # ", " # # "}},
        {' ', {"     ", "     ", "     ", "     ", "     ", "     ", "     "}},
        {'?', {" ### ", "#   #", "    #", "   # ", "  #  ", "     ", "  #  "}},
    };
    return f;
}

struct Rgba {
    uint8_t r, g, b, a;
};

struct Image {
    uint32_t w, h;
    std::vector<uint8_t> px;
    Image(uint32_t width, uint32_t height, Rgba fill) : w(width), h(height), px(size_t(width) * height * 4) {
        for (size_t i = 0; i < size_t(w) * h; ++i) {
            px[i * 4] = fill.r;
            px[i * 4 + 1] = fill.g;
            px[i * 4 + 2] = fill.b;
            px[i * 4 + 3] = fill.a;
        }
    }
    void Set(int x, int y, Rgba c) {
        if (x < 0 || y < 0 || x >= int(w) || y >= int(h)) return;
        auto* p = &px[(size_t(y) * w + size_t(x)) * 4];
        p[0] = c.r;
        p[1] = c.g;
        p[2] = c.b;
        p[3] = c.a;
    }
    void Fill(int x0, int y0, int x1, int y1, Rgba c) {
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) Set(x, y, c);
    }
};

std::string Upper(std::string s) {
    for (auto& ch : s) {
        if (ch >= 'a' && ch <= 'z') ch = char(ch - 'a' + 'A');
        if (!Font().count(ch)) ch = '?';
    }
    return s;
}

int TextWidth(const std::string& s, int scale) { return s.empty() ? 0 : int(s.size()) * 6 * scale - scale; }

// Largest integer scale (<= max_scale) that fits the box; at least 1.
int FitScale(const std::string& s, int box_w, int box_h, int max_scale) {
    int scale = max_scale;
    while (scale > 1 && (TextWidth(s, scale) > box_w || 7 * scale > box_h)) --scale;
    return scale;
}

void DrawText(Image& img, const std::string& text, int cx, int cy, int scale, Rgba color) {
    const std::string s = Upper(text);
    int x = cx - TextWidth(s, scale) / 2;
    const int y = cy - 7 * scale / 2;
    for (char ch : s) {
        const auto& g = Font().at(ch);
        for (int row = 0; row < 7; ++row)
            for (int col = 0; col < 5; ++col)
                if (g[size_t(row)][col] == '#') img.Fill(x + col * scale, y + row * scale, x + (col + 1) * scale, y + (row + 1) * scale, color);
        x += 6 * scale;
    }
}

// --- PNG -------------------------------------------------------------------
uint32_t Crc32(const uint8_t* data, size_t n, uint32_t crc = 0) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void PutBE(std::vector<uint8_t>& v, uint32_t x) {
    for (int s = 24; s >= 0; s -= 8) v.push_back(uint8_t(x >> s));
}

void Chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    PutBE(out, uint32_t(data.size()));
    std::vector<uint8_t> td(type, type + 4);
    td.insert(td.end(), data.begin(), data.end());
    out.insert(out.end(), td.begin(), td.end());
    PutBE(out, Crc32(td.data(), td.size()));
}

// --- Mesh building -----------------------------------------------------------
struct V3 {
    double x, y, z;
};

struct Mesh {
    std::vector<V3> v, n;
    std::vector<std::pair<double, double>> uv;
    std::vector<std::array<int, 3>> f; // 1-based indices shared by v/vt/vn

    // Quad p0..p3 with UVs; flips the winding if it does not face `normal`.
    void Quad(std::array<V3, 4> p, std::array<std::pair<double, double>, 4> t, V3 normal) {
        const V3 a{p[1].x - p[0].x, p[1].y - p[0].y, p[1].z - p[0].z};
        const V3 b{p[2].x - p[0].x, p[2].y - p[0].y, p[2].z - p[0].z};
        const V3 c{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
        if (c.x * normal.x + c.y * normal.y + c.z * normal.z < 0) {
            std::swap(p[1], p[3]);
            std::swap(t[1], t[3]);
        }
        const int base = int(v.size()) + 1;
        for (int i = 0; i < 4; ++i) {
            v.push_back(p[size_t(i)]);
            n.push_back(normal);
            uv.push_back(t[size_t(i)]);
        }
        f.push_back({base, base + 1, base + 2});
        f.push_back({base, base + 2, base + 3});
    }

    std::string Obj() const {
        std::string s = "mtllib body.mtl\nusemtl body\n";
        char buf[128];
        for (const auto& p : v) {
            std::snprintf(buf, sizeof(buf), "v %.5f %.5f %.5f\n", p.x, p.y, p.z);
            s += buf;
        }
        for (const auto& t : uv) {
            std::snprintf(buf, sizeof(buf), "vt %.4f %.4f\n", t.first, t.second);
            s += buf;
        }
        for (const auto& q : n) {
            std::snprintf(buf, sizeof(buf), "vn %.4f %.4f %.4f\n", q.x, q.y, q.z);
            s += buf;
        }
        for (const auto& t : f) {
            std::snprintf(buf, sizeof(buf), "f %d/%d/%d %d/%d/%d %d/%d/%d\n", t[0], t[0], t[0], t[1], t[1], t[1], t[2], t[2], t[2]);
            s += buf;
        }
        return s;
    }
};

constexpr int kTex = 256;
double V(int row) { return 1.0 - double(row) / kTex; } // image row -> OBJ v

const Rgba kLabelBg{36, 40, 48, 255};
const Rgba kLabelFg{232, 234, 238, 255};
const Rgba kLabelSub{150, 160, 175, 255};

void AddBox(Mesh& m, V3 lo, V3 hi, std::pair<double, double> uv) {
    const auto q = [&](V3 a, V3 b, V3 c, V3 d, V3 nrm) { m.Quad({a, b, c, d}, {uv, uv, uv, uv}, nrm); };
    q({lo.x, hi.y, lo.z}, {lo.x, hi.y, hi.z}, {hi.x, hi.y, hi.z}, {hi.x, hi.y, lo.z}, {0, 1, 0});
    q({lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, lo.y, hi.z}, {lo.x, lo.y, hi.z}, {0, -1, 0});
    q({lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}, {0, 0, 1});
    q({lo.x, lo.y, lo.z}, {lo.x, hi.y, lo.z}, {hi.x, hi.y, lo.z}, {hi.x, lo.y, lo.z}, {0, 0, -1});
    q({hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {hi.x, hi.y, hi.z}, {hi.x, lo.y, hi.z}, {1, 0, 0});
    q({lo.x, lo.y, lo.z}, {lo.x, lo.y, hi.z}, {lo.x, hi.y, hi.z}, {lo.x, hi.y, lo.z}, {-1, 0, 0});
}

} // namespace

DeviceLabel TrackerLabel(const std::string& role, const std::string& id) {
    static const std::map<std::string, std::pair<std::string, std::string>> names = {
        {"left_foot", {"LF", "L FOOT"}},         {"right_foot", {"RF", "R FOOT"}},
        {"left_ankle", {"LA", "L ANKLE"}},       {"right_ankle", {"RA", "R ANKLE"}},
        {"left_knee", {"LK", "L KNEE"}},         {"right_knee", {"RK", "R KNEE"}},
        {"left_elbow", {"LE", "L ELBOW"}},       {"right_elbow", {"RE", "R ELBOW"}},
        {"left_wrist", {"LW", "L WRIST"}},       {"right_wrist", {"RW", "R WRIST"}},
        {"left_shoulder", {"LS", "L SHOULDER"}}, {"right_shoulder", {"RS", "R SHOULDER"}},
        {"waist", {"W", "WAIST"}},               {"chest", {"C", "CHEST"}},
        {"camera", {"CM", "CAMERA"}},            {"keyboard", {"KB", "KEYBOARD"}},
        {"handed", {"HD", "HANDED"}},
    };
    DeviceLabel l;
    l.subtitle = id;
    if (auto it = names.find(role); it != names.end()) {
        l.icon_text = it->second.first;
        l.title = it->second.second;
    } else {
        // No role: show the tracker number ("tracker_3" -> "T3").
        const auto digits = id.find_first_of("0123456789");
        l.icon_text = digits == std::string::npos ? "T" : "T" + id.substr(digits, 1);
        l.title = "TRACKER";
    }
    return l;
}

DeviceLabel ControllerLabel(bool left, const std::string& id) {
    return DeviceLabel{left ? "L" : "R", left ? "L" : "R", id};
}

const char* IconStateName(IconState s) {
    switch (s) {
    case IconState::Off: return "off";
    case IconState::Searching: return "searching";
    case IconState::SearchingAlert: return "searching_alert";
    case IconState::Ready: return "ready";
    case IconState::ReadyAlert: return "ready_alert";
    case IconState::NotReady: return "error";
    case IconState::Standby: return "standby";
    case IconState::AlertLow: return "ready_low";
    }
    return "ready";
}

std::vector<uint8_t> MakeStatusIcon(const std::string& text, IconState state) {
    const Rgba green{140, 198, 63, 255}, gray{110, 110, 110, 255}, yellow{230, 190, 50, 255}, red{205, 70, 60, 255},
        orange{230, 140, 50, 255}, dark{30, 32, 36, 255};
    Rgba color = green;
    bool filled = true;
    switch (state) {
    case IconState::Ready: break;
    case IconState::ReadyAlert: color = yellow; break;
    case IconState::AlertLow: color = orange; break;
    case IconState::NotReady: color = red; break;
    case IconState::Off: color = gray; break;
    case IconState::Standby: filled = false; break;
    case IconState::Searching: color = gray; filled = false; break;
    case IconState::SearchingAlert: color = yellow; filled = false; break;
    }
    // Flat-top hexagon, 4x4 supersampled for smooth edges.
    Image img(32, 32, {0, 0, 0, 0});
    const double cx = 15.5, cy = 15.5, r = 15.0, ri = 12.6;
    const auto inside = [&](double x, double y, double rad) {
        const double dx = std::abs(x - cx), dy = std::abs(y - cy);
        const double h = rad * std::sqrt(3.0) / 2;
        return dy <= h && dx <= rad - dy / std::sqrt(3.0);
    };
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            int cover = 0;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx) {
                    const double px = x + (sx + 0.5) / 4, py = y + (sy + 0.5) / 4;
                    const bool in = inside(px, py, r) && (filled || !inside(px, py, ri));
                    cover += in;
                }
            if (cover) img.Set(x, y, {color.r, color.g, color.b, uint8_t(cover * 255 / 16)});
        }
    }
    const std::string t = Upper(text);
    const int scale = FitScale(t, 22, 18, 3);
    DrawText(img, t, 16, 16, scale, filled ? dark : color);
    return EncodePng(img.w, img.h, img.px);
}

RenderModelFiles MakeRenderModel(ModelShape shape, const DeviceLabel& label) {
    RenderModelFiles out;
    out.mtl = "newmtl body\nKa 1 1 1\nKd 1 1 1\nmap_Kd body.png\n";
    Image tex(kTex, kTex, kLabelBg);
    Mesh m;

    if (shape == ModelShape::Tracker) {
        // Texture: rows 0-159 top label (title + id), 160-223 side label (title), 224-255 body color.
        const std::string title = Upper(label.title), sub = Upper(label.subtitle);
        DrawText(tex, title, kTex / 2, 62, FitScale(title, 236, 80, 6), kLabelFg);
        DrawText(tex, sub, kTex / 2, 128, FitScale(sub, 236, 30, 3), kLabelSub);
        tex.Fill(0, 158, kTex, 160, {70, 76, 88, 255});
        DrawText(tex, title, kTex / 2, 192, FitScale(title, 244, 52, 6), kLabelFg);
        tex.Fill(0, 224, kTex, kTex, {88, 92, 102, 255});
        const double a = 0.035, h = 0.0125;
        const std::pair<double, double> body{0.5, V(240)};
        // Top: text runs along +X, its top towards -Z (readable for someone standing at +Z).
        m.Quad({V3{-a, h, -a}, {a, h, -a}, {a, h, a}, {-a, h, a}}, {{{0, V(0)}, {1, V(0)}, {1, V(158)}, {0, V(158)}}}, {0, 1, 0});
        const double t = V(160), b = V(224);
        m.Quad({V3{-a, -h, a}, {a, -h, a}, {a, h, a}, {-a, h, a}}, {{{0, b}, {1, b}, {1, t}, {0, t}}}, {0, 0, 1});
        m.Quad({V3{a, -h, -a}, {-a, -h, -a}, {-a, h, -a}, {a, h, -a}}, {{{0, b}, {1, b}, {1, t}, {0, t}}}, {0, 0, -1});
        m.Quad({V3{a, -h, a}, {a, -h, -a}, {a, h, -a}, {a, h, a}}, {{{0, b}, {1, b}, {1, t}, {0, t}}}, {1, 0, 0});
        m.Quad({V3{-a, -h, -a}, {-a, -h, a}, {-a, h, a}, {-a, h, -a}}, {{{0, b}, {1, b}, {1, t}, {0, t}}}, {-1, 0, 0});
        m.Quad({V3{-a, -h, -a}, {a, -h, -a}, {a, -h, a}, {-a, -h, a}}, {body, body, body, body}, {0, -1, 0});
        out.json = R"({
  "thumbnail": "body.png",
  "components": {
    "body": { "filename": "body.obj", "component_local": { "origin": [0, 0, 0], "rotate_xyz": [0, 0, 0] } }
  }
}
)";
    } else {
        // Texture: rows 0-191 label plate (big L/R + id), 192-255 body color.
        const std::string title = Upper(label.title), sub = Upper(label.subtitle);
        DrawText(tex, title, kTex / 2, 78, FitScale(title, 236, 120, 16), kLabelFg);
        DrawText(tex, sub, kTex / 2, 166, FitScale(sub, 236, 30, 3), kLabelSub);
        tex.Fill(0, 192, kTex, kTex, {70, 110, 140, 255});
        const std::pair<double, double> body{0.5, V(224)};
        // Capsule along Z (tip at -Z), radius 18 mm.
        const double radius = 0.018, half = 0.06, zc = 0.02;
        const int rings = 8, segs = 16;
        std::vector<std::vector<int>> rows;
        for (int i = 0; i <= rings * 2 + 1; ++i) {
            double z, r, nz;
            if (i <= rings) {
                const double ang = 3.14159265358979 / 2 * (1.0 - double(i) / rings);
                z = zc - half - radius * std::sin(ang);
                r = radius * std::cos(ang);
                nz = -std::sin(ang);
            } else {
                const double ang = 3.14159265358979 / 2 * (double(i - rings - 1) / rings);
                z = zc + half + radius * std::sin(ang);
                r = radius * std::cos(ang);
                nz = std::sin(ang);
            }
            std::vector<int> row;
            for (int j = 0; j <= segs; ++j) {
                const double th = 2 * 3.14159265358979 * j / segs, nr = std::sqrt(std::max(0.0, 1 - nz * nz));
                m.v.push_back({r * std::cos(th), r * std::sin(th), z});
                m.n.push_back({nr * std::cos(th), nr * std::sin(th), nz});
                m.uv.push_back(body);
                row.push_back(int(m.v.size()));
            }
            rows.push_back(row);
        }
        for (size_t i = 0; i + 1 < rows.size(); ++i)
            for (int j = 0; j < segs; ++j) {
                m.f.push_back({rows[i][size_t(j)], rows[i + 1][size_t(j)], rows[i + 1][size_t(j) + 1]});
                m.f.push_back({rows[i][size_t(j)], rows[i + 1][size_t(j) + 1], rows[i][size_t(j) + 1]});
            }
        // Label plate on top, behind the tip: 50 x 37.5 mm (same aspect as its texture area).
        const double px = 0.025, y0 = 0.0175, y1 = 0.021, z0 = -0.005, z1 = 0.0325;
        AddBox(m, {-px, y0, z0}, {px, y1 - 0.0001, z1}, body);
        m.Quad({V3{-px, y1, z0}, {px, y1, z0}, {px, y1, z1}, {-px, y1, z1}}, {{{0, V(0)}, {1, V(0)}, {1, V(192)}, {0, V(192)}}}, {0, 1, 0});
        out.json = R"({
  "thumbnail": "body.png",
  "components": {
    "body": { "filename": "body.obj", "component_local": { "origin": [0, 0, 0], "rotate_xyz": [0, 0, 0] } },
    "tip": { "component_local": { "origin": [0, 0, -0.06], "rotate_xyz": [0, 0, 0] } },
    "handgrip": { "component_local": { "origin": [0, 0, 0.03], "rotate_xyz": [0, 0, 0] } },
    "base": { "component_local": { "origin": [0, -0.02, 0.10], "rotate_xyz": [0, 180, 0] } },
    "openxr_grip": { "component_local": { "origin": [0, 0, 0.03], "rotate_xyz": [0, 0, 0] } },
    "openxr_aim": { "component_local": { "origin": [0, 0, -0.06], "rotate_xyz": [0, 0, 0] } }
  }
}
)";
    }
    out.obj = m.Obj();
    out.texture = EncodePng(tex.w, tex.h, tex.px);
    return out;
}

std::vector<uint8_t> EncodePng(uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba) {
    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<uint8_t> ihdr;
    PutBE(ihdr, width);
    PutBE(ihdr, height);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0}); // 8-bit RGBA
    Chunk(out, "IHDR", ihdr);

    std::vector<uint8_t> raw;
    raw.reserve((size_t(width) * 4 + 1) * height);
    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0); // filter: none
        raw.insert(raw.end(), rgba.begin() + std::ptrdiff_t(size_t(y) * width * 4),
                   rgba.begin() + std::ptrdiff_t(size_t(y + 1) * width * 4));
    }
    // zlib stream with stored (uncompressed) deflate blocks.
    std::vector<uint8_t> z = {0x78, 0x01};
    size_t pos = 0;
    do {
        const size_t len = std::min<size_t>(65535, raw.size() - pos);
        z.push_back(pos + len == raw.size() ? 1 : 0);
        z.push_back(uint8_t(len));
        z.push_back(uint8_t(len >> 8));
        z.push_back(uint8_t(~len));
        z.push_back(uint8_t(~len >> 8));
        z.insert(z.end(), raw.begin() + std::ptrdiff_t(pos), raw.begin() + std::ptrdiff_t(pos + len));
        pos += len;
    } while (pos < raw.size());
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    PutBE(z, (b << 16) | a);
    Chunk(out, "IDAT", z);
    Chunk(out, "IEND", {});
    return out;
}

} // namespace vhmd
