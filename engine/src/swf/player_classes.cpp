// MovieClip, TextField, Color and flash.geom for the player's scripts.
#include <algorithm>
#include <cmath>

#include "swf/player.h"

namespace tdkr::swf {

namespace {

constexpr float kTwips = 20.0f;

Instance* self_of(CallArgs& a) { return a.self.instance(); }

int frame_arg(VM& vm, Instance* i, const Value& v) {
    if (v.type == Value::String) {
        const int f = i->find_label(v.s);
        if (f >= 0) return f;
        const double n = vm.to_number(v);
        return std::isnan(n) ? -1 : int(n) - 1;
    }
    const double n = vm.to_number(v);
    return std::isnan(n) ? -1 : int(n) - 1;
}

ObjectPtr rect_object(VM& vm, const Rect& r) {
    auto o = vm.new_object();
    o->set_own("xMin", Value(double(r.xmin / kTwips)));
    o->set_own("xMax", Value(double(r.xmax / kTwips)));
    o->set_own("yMin", Value(double(r.ymin / kTwips)));
    o->set_own("yMax", Value(double(r.ymax / kTwips)));
    return o;
}

void insert_child(Instance* parent, const std::shared_ptr<Instance>& inst) {
    auto it = std::lower_bound(parent->children.begin(), parent->children.end(), inst->depth,
                               [](const std::shared_ptr<Instance>& c, int d) { return c->depth < d; });
    parent->children.insert(it, inst);
}

}  // namespace

void Player::install_classes() {
    VM& v = vm;
    const ObjectPtr mc = vm.movieclip_proto;
    auto klass = [&](const std::string& name, const ObjectPtr& proto, NativeFunction::Fn fn, NativeFunction::Ctor ctor = nullptr) {
        auto f = vm.native(std::move(fn), std::move(ctor));
        f->set_own("prototype", Value(proto), 1);
        proto->set_own("constructor", Value(ObjectPtr(f)), 1);
        vm.global->set_own(name, Value(ObjectPtr(f)), 1);
        return f;
    };
    klass("MovieClip", mc, [](VM&, CallArgs&) { return Value(); });
    klass("TextField", vm.textfield_proto, [](VM&, CallArgs&) { return Value(); });

    // --- timeline
    v.method(mc, "play", [](VM&, CallArgs& a) {
        if (Instance* i = self_of(a)) i->playing = true;
        return Value();
    });
    v.method(mc, "stop", [](VM&, CallArgs& a) {
        if (Instance* i = self_of(a)) i->playing = false;
        return Value();
    });
    v.method(mc, "nextFrame", [](VM&, CallArgs& a) {
        if (Instance* i = self_of(a)) i->goto_frame(i->frame + 1, false);
        return Value();
    });
    v.method(mc, "prevFrame", [](VM&, CallArgs& a) {
        if (Instance* i = self_of(a)) i->goto_frame(std::max(0, i->frame - 1), false);
        return Value();
    });
    v.method(mc, "gotoAndPlay", [](VM& vm, CallArgs& a) {
        if (Instance* i = self_of(a)) {
            const int f = frame_arg(vm, i, a[0]);
            if (f >= 0) i->goto_frame(f, true);
        }
        return Value();
    });
    v.method(mc, "gotoAndStop", [](VM& vm, CallArgs& a) {
        if (Instance* i = self_of(a)) {
            const int f = frame_arg(vm, i, a[0]);
            if (f >= 0) i->goto_frame(f, false);
        }
        return Value();
    });

    // --- children
    v.method(mc, "attachMovie", [this](VM& vm, CallArgs& a) {
        Instance* i = self_of(a);
        if (!i || !i->movie) return Value();
        const uint16_t id = i->movie->exported(vm.to_string(a[0]));
        if (!id) {
            if (log) log("swf: attachMovie: no symbol " + vm.to_string(a[0]) + " in " + i->movie->name);
            return Value();
        }
        const int depth = int(vm.to_number(a[2]));
        remove_depth(i, depth);
        auto inst = create(i, i->movie, id, depth, vm.to_string(a[1]));
        if (!inst) return Value();
        construct(inst, a[3]);
        return Value(ObjectPtr(inst));
    });
    v.method(mc, "createEmptyMovieClip", [this](VM& vm, CallArgs& a) {
        Instance* i = self_of(a);
        if (!i) return Value();
        const int depth = int(vm.to_number(a[1]));
        remove_depth(i, depth);
        auto inst = std::make_shared<Instance>(*this, Instance::Clip, i->movie, nullptr, vm.movieclip_proto);
        inst->name = vm.to_string(a[0]);
        inst->depth = depth;
        inst->parent_ = i;
        insert_child(i, inst);
        construct(inst);
        return Value(ObjectPtr(inst));
    });
    v.method(mc, "createTextField", [this](VM& vm, CallArgs& a) {
        Instance* i = self_of(a);
        if (!i) return Value();
        const int depth = int(vm.to_number(a[1]));
        remove_depth(i, depth);
        auto def = std::make_shared<EditTextDef>();
        def->bounds = Rect{0, float(vm.to_number(a[4])) * kTwips, 0, float(vm.to_number(a[5])) * kTwips};
        def->height = 12 * kTwips;
        auto inst = std::make_shared<Instance>(*this, Instance::Text, i->movie, def.get(), vm.textfield_proto);
        inst->own_text = def;
        inst->name = vm.to_string(a[0]);
        inst->depth = depth;
        inst->parent_ = i;
        inst->matrix.tx = float(vm.to_number(a[2])) * kTwips;
        inst->matrix.ty = float(vm.to_number(a[3])) * kTwips;
        inst->text_width = float(vm.to_number(a[4]));
        inst->text_height = float(vm.to_number(a[5]));
        inst->text_size = 12;
        inst->text_font = "_sans";
        inst->constructed = true;
        insert_child(i, inst);
        return Value(ObjectPtr(inst));
    });
    v.method(mc, "duplicateMovieClip", [this](VM& vm, CallArgs& a) {
        Instance* i = self_of(a);
        if (!i || !i->parent_) return Value();
        const int depth = int(vm.to_number(a[1]));
        remove_depth(i->parent_, depth);
        auto copy = create(i->parent_, i->movie, i->character, depth, vm.to_string(a[0]));
        if (!copy) return Value();
        copy->matrix = i->matrix;
        copy->cxform = i->cxform;
        copy->clip_events = i->clip_events;
        construct(copy, a[2]);
        return Value(ObjectPtr(copy));
    });
    auto remove = [this](VM&, CallArgs& a) {
        if (Instance* i = self_of(a)) i->remove_self();
        return Value();
    };
    v.method(mc, "removeMovieClip", remove);
    v.method(vm.textfield_proto, "removeTextField", remove);
    v.method(mc, "unloadMovie", [this](VM&, CallArgs& a) {
        if (Instance* i = self_of(a)) {
            for (auto& c : i->children) unload(c.get());
            i->children.clear();
        }
        return Value();
    });
    v.method(mc, "loadMovie", [this](VM& vm, CallArgs& a) {
        Instance* i = self_of(a);
        if (i) load_movie(i->target_path(), vm.to_string(a[0]));
        return Value();
    });
    auto swap = [](VM& vm, CallArgs& a) {
        Instance* i = self_of(a);
        if (!i || !i->parent_) return Value();
        Instance* p = i->parent_;
        int target = 0;
        Instance* other = a[0].instance();
        if (other) {
            if (other->parent_ != p) return Value();
            target = other->depth;
        } else {
            target = int(vm.to_number(a[0]));
            other = p->child_at(target);
        }
        if (other == i) return Value();
        if (other) other->depth = i->depth;
        i->depth = target;
        i->accept_moves = false;
        if (other) other->accept_moves = false;
        std::stable_sort(p->children.begin(), p->children.end(),
                         [](const std::shared_ptr<Instance>& x, const std::shared_ptr<Instance>& y) { return x->depth < y->depth; });
        return Value();
    };
    v.method(mc, "swapDepths", swap);
    v.method(vm.textfield_proto, "swapDepths", swap);
    v.method(mc, "getNextHighestDepth", [](VM&, CallArgs& a) {
        Instance* i = self_of(a);
        int d = 0;
        if (i)
            for (const auto& c : i->children) d = std::max(d, c->depth + 1);
        return Value(d);
    });
    auto get_depth = [](VM&, CallArgs& a) {
        Instance* i = self_of(a);
        return i ? Value(i->depth) : Value();
    };
    v.method(mc, "getDepth", get_depth);
    v.method(vm.textfield_proto, "getDepth", get_depth);
    v.method(mc, "getInstanceAtDepth", [](VM& vm, CallArgs& a) {
        Instance* i = self_of(a);
        Instance* c = i ? i->child_at(int(vm.to_number(a[0]))) : nullptr;
        return c ? Value(c->shared_from_this()) : Value();
    });
    v.method(mc, "getSWFVersion", [](VM& vm, CallArgs&) { return Value(vm.version); });
    v.method(mc, "getBytesLoaded", [](VM&, CallArgs&) { return Value(1); });
    v.method(mc, "getBytesTotal", [](VM&, CallArgs&) { return Value(1); });
    for (const char* n : {"startDrag", "stopDrag", "attachAudio", "attachBitmap", "setScale9Grid"})
        v.method(mc, n, [](VM&, CallArgs&) { return Value(); });
    v.method(mc, "getURL", [](VM& vm, CallArgs& a) {
        const std::string url = vm.to_string(a[0]);
        if (url.rfind("FSCommand:", 0) == 0 && vm.fscommand) vm.fscommand(url.substr(10), vm.to_string(a[1]));
        return Value();
    });

    // --- geometry
    auto bounds_in = [](VM& vm, CallArgs& a) {
        Instance* i = self_of(a);
        if (!i) return Value();
        Instance* space = a[0].instance() ? a[0].instance() : i;
        const Matrix m = space->world_matrix().inverse() * i->world_matrix();
        return Value(rect_object(vm, i->bounds(m)));
    };
    v.method(mc, "getBounds", bounds_in);
    v.method(mc, "getRect", bounds_in);
    v.method(mc, "localToGlobal", [](VM& vm, CallArgs& a) {
        Instance* i = self_of(a);
        if (!i || !a[0].is_object()) return Value();
        const glm::vec2 p(float(vm.to_number(vm.get_member(a[0], "x"))), float(vm.to_number(vm.get_member(a[0], "y"))));
        const glm::vec2 g = i->world_matrix().apply(p * kTwips) / kTwips;
        vm.set_member(a[0], "x", Value(double(g.x)));
        vm.set_member(a[0], "y", Value(double(g.y)));
        return Value();
    });
    v.method(mc, "globalToLocal", [](VM& vm, CallArgs& a) {
        Instance* i = self_of(a);
        if (!i || !a[0].is_object()) return Value();
        const glm::vec2 p(float(vm.to_number(vm.get_member(a[0], "x"))), float(vm.to_number(vm.get_member(a[0], "y"))));
        const glm::vec2 l = i->world_matrix().inverse().apply(p * kTwips) / kTwips;
        vm.set_member(a[0], "x", Value(double(l.x)));
        vm.set_member(a[0], "y", Value(double(l.y)));
        return Value();
    });
    v.method(mc, "hitTest", [](VM& vm, CallArgs& a) {
        Instance* i = self_of(a);
        if (!i) return Value(false);
        const Rect b = i->bounds(i->world_matrix());
        if (Instance* o = a[0].instance()) {
            const Rect c = o->bounds(o->world_matrix());
            return Value(!(c.xmin > b.xmax || c.xmax < b.xmin || c.ymin > b.ymax || c.ymax < b.ymin));
        }
        const float x = float(vm.to_number(a[0])) * kTwips, y = float(vm.to_number(a[1])) * kTwips;
        return Value(x >= b.xmin && x <= b.xmax && y >= b.ymin && y <= b.ymax);
    });
    v.method(mc, "setMask", [](VM&, CallArgs& a) {
        Instance* i = self_of(a);
        if (!i) return Value();
        Instance* m = a[0].instance();
        i->mask = m ? std::static_pointer_cast<Instance>(m->shared_from_this()) : nullptr;
        if (m) m->visible = true;
        return Value();
    });

    // --- drawing API (a few HUD bars)
    auto new_group = [](Instance* i) {
        i->drawing.emplace_back();
        ShapeGroup& g = i->drawing.back();
        if (i->has_line) {
            g.lines.push_back(i->line_style);
            g.line_edges.resize(1);
        }
    };
    v.method(mc, "clear", [this](VM&, CallArgs& a) {
        if (Instance* i = self_of(a)) {
            i->drawing.clear();
            i->filling = false;
            mesh_cache_.erase(&i->drawing);
        }
        return Value();
    });
    v.method(mc, "lineStyle", [new_group](VM& vm, CallArgs& a) {
        Instance* i = self_of(a);
        if (!i) return Value();
        i->has_line = !a[0].undefined();
        const uint32_t c = uint32_t(vm.to_number(a[1]));
        const double alpha = a[2].undefined() ? 100 : vm.to_number(a[2]);
        i->line_style.width = float(vm.to_number(a[0])) * kTwips;
        i->line_style.color = glm::vec4(float(c >> 16 & 255) / 255.0f, float(c >> 8 & 255) / 255.0f, float(c & 255) / 255.0f, float(alpha / 100));
        new_group(i);
        return Value();
    });
    v.method(mc, "beginFill", [new_group](VM& vm, CallArgs& a) {
        Instance* i = self_of(a);
        if (!i) return Value();
        new_group(i);
        FillStyle fs;
        const uint32_t c = uint32_t(vm.to_number(a[0]));
        const double alpha = a[1].undefined() ? 100 : vm.to_number(a[1]);
        fs.color = glm::vec4(float(c >> 16 & 255) / 255.0f, float(c >> 8 & 255) / 255.0f, float(c & 255) / 255.0f, float(alpha / 100));
        i->drawing.back().fills.push_back(fs);
        i->drawing.back().fill_edges.resize(1);
        i->filling = true;
        i->fill_start = i->pen;
        return Value();
    });
    auto add_edge = [this](Instance* i, const Edge& e) {
        if (i->drawing.empty()) {
            i->drawing.emplace_back();
            if (i->has_line) {
                i->drawing.back().lines.push_back(i->line_style);
                i->drawing.back().line_edges.resize(1);
            }
        }
        ShapeGroup& g = i->drawing.back();
        if (i->filling && !g.fill_edges.empty()) g.fill_edges[0].push_back(e);
        if (!g.line_edges.empty()) g.line_edges[0].push_back(e);
        mesh_cache_.erase(&i->drawing);
        i->pen = e.p1;
    };
    v.method(mc, "moveTo", [](VM& vm, CallArgs& a) {
        if (Instance* i = self_of(a)) {
            i->pen = glm::vec2(float(vm.to_number(a[0])), float(vm.to_number(a[1]))) * kTwips;
            if (i->filling) i->fill_start = i->pen;
        }
        return Value();
    });
    v.method(mc, "lineTo", [add_edge](VM& vm, CallArgs& a) {
        if (Instance* i = self_of(a)) {
            const glm::vec2 p = glm::vec2(float(vm.to_number(a[0])), float(vm.to_number(a[1]))) * kTwips;
            add_edge(i, Edge{i->pen, (i->pen + p) * 0.5f, p, false});
        }
        return Value();
    });
    v.method(mc, "curveTo", [add_edge](VM& vm, CallArgs& a) {
        if (Instance* i = self_of(a)) {
            const glm::vec2 c = glm::vec2(float(vm.to_number(a[0])), float(vm.to_number(a[1]))) * kTwips;
            const glm::vec2 p = glm::vec2(float(vm.to_number(a[2])), float(vm.to_number(a[3]))) * kTwips;
            add_edge(i, Edge{i->pen, c, p, true});
        }
        return Value();
    });
    v.method(mc, "endFill", [add_edge](VM&, CallArgs& a) {
        if (Instance* i = self_of(a); i && i->filling) {
            if (i->pen != i->fill_start) add_edge(i, Edge{i->pen, (i->pen + i->fill_start) * 0.5f, i->fill_start, false});
            i->filling = false;
        }
        return Value();
    });
    v.method(mc, "beginGradientFill", [](VM&, CallArgs&) { return Value(); });

    // --- TextField
    const ObjectPtr tf = vm.textfield_proto;
    auto set_format = [](VM& vm, CallArgs& a) {
        Instance* i = self_of(a);
        if (!i || i->kind != Instance::Text) return Value();
        const Value fmt = a.size() >= 3 ? a[2] : a.size() == 2 ? a[1] : a[0];
        if (!fmt.is_object()) return Value();
        const Value color = vm.get_member(fmt, "color"), size = vm.get_member(fmt, "size"),
                    align = vm.get_member(fmt, "align"), font = vm.get_member(fmt, "font");
        if (!color.nullish()) {
            const uint32_t c = uint32_t(vm.to_number(color));
            i->text_color = glm::vec4(float(c >> 16 & 255) / 255.0f, float(c >> 8 & 255) / 255.0f, float(c & 255) / 255.0f, 1.0f);
        }
        if (!size.nullish()) i->text_size = float(vm.to_number(size));
        if (!align.nullish()) {
            const std::string s = vm.to_string(align);
            i->text_align = s == "right" ? 1 : s == "center" ? 2 : 0;
        }
        if (!font.nullish()) i->text_font = vm.to_string(font);
        return Value();
    };
    v.method(tf, "setTextFormat", set_format);
    v.method(tf, "setNewTextFormat", set_format);
    auto get_format = [](VM& vm, CallArgs& a) {
        Instance* i = self_of(a);
        auto o = std::make_shared<Object>(vm.textformat_proto);
        if (!i) return Value(ObjectPtr(o));
        const glm::ivec4 c = glm::ivec4(i->text_color * 255.0f + 0.5f);
        o->set_own("color", Value(double(c.r << 16 | c.g << 8 | c.b)));
        o->set_own("size", Value(double(i->text_size)));
        o->set_own("font", Value(i->text_font));
        o->set_own("align", Value(std::string(i->text_align == 1 ? "right" : i->text_align == 2 ? "center" : "left")));
        o->set_own("bold", Value(false));
        o->set_own("italic", Value(false));
        o->set_own("leading", Value(0));
        return Value(ObjectPtr(o));
    };
    v.method(tf, "getTextFormat", get_format);
    v.method(tf, "getNewTextFormat", get_format);
    v.method(tf, "replaceSel", [](VM&, CallArgs&) { return Value(); });

    // --- Color
    auto color_proto = vm.new_object();
    auto target_of = [this](VM& vm, const Value& self) -> Instance* {
        const Value t = vm.get_member(self, "_target_clip");
        if (Instance* i = t.instance()) return i;
        return t.type == Value::String ? find(t.s) : nullptr;
    };
    klass("Color", color_proto, [](VM&, CallArgs&) { return Value(); }, [color_proto](VM&, CallArgs& a) -> ObjectPtr {
        auto o = std::make_shared<Object>(color_proto);
        o->set_own("_target_clip", a[0], 1);
        return o;
    });
    v.method(color_proto, "setRGB", [target_of](VM& vm, CallArgs& a) {
        if (Instance* i = target_of(vm, a.self)) {
            const uint32_t c = uint32_t(vm.to_number(a[0]));
            i->cxform.mul = glm::vec4(0, 0, 0, i->cxform.mul.a);
            i->cxform.add = glm::vec4(float(c >> 16 & 255) / 255.0f, float(c >> 8 & 255) / 255.0f, float(c & 255) / 255.0f, i->cxform.add.a);
        }
        return Value();
    });
    v.method(color_proto, "getRGB", [target_of](VM& vm, CallArgs& a) {
        Instance* i = target_of(vm, a.self);
        if (!i) return Value(0);
        const glm::ivec4 c = glm::ivec4(glm::clamp(i->cxform.add, 0.0f, 1.0f) * 255.0f + 0.5f);
        return Value(double(c.r << 16 | c.g << 8 | c.b));
    });
    v.method(color_proto, "setTransform", [target_of](VM& vm, CallArgs& a) {
        Instance* i = target_of(vm, a.self);
        if (!i || !a[0].is_object()) return Value();
        const char* mul[] = {"ra", "ga", "ba", "aa"};
        const char* add[] = {"rb", "gb", "bb", "ab"};
        for (int k = 0; k < 4; ++k) {
            const Value m = vm.get_member(a[0], mul[k]), o = vm.get_member(a[0], add[k]);
            if (!m.undefined()) i->cxform.mul[k] = float(vm.to_number(m) / 100);
            if (!o.undefined()) i->cxform.add[k] = float(vm.to_number(o) / 255);
        }
        return Value();
    });
    v.method(color_proto, "getTransform", [target_of](VM& vm, CallArgs& a) {
        auto o = vm.new_object();
        if (Instance* i = target_of(vm, a.self)) {
            const char* mul[] = {"ra", "ga", "ba", "aa"};
            const char* add[] = {"rb", "gb", "bb", "ab"};
            for (int k = 0; k < 4; ++k) {
                o->set_own(mul[k], Value(double(i->cxform.mul[k] * 100)));
                o->set_own(add[k], Value(double(i->cxform.add[k] * 255)));
            }
        }
        return Value(o);
    });

    // --- flash.geom: Point, ColorTransform, Transform
    auto flash = vm.new_object();
    auto geom = vm.new_object();
    flash->set_own("geom", Value(geom));
    vm.global->set_own("flash", Value(flash), 1);
    auto geom_class = [&](const std::string& name, const ObjectPtr& proto, NativeFunction::Ctor ctor) {
        auto f = vm.native([](VM&, CallArgs&) { return Value(); }, std::move(ctor));
        f->set_own("prototype", Value(proto), 1);
        proto->set_own("constructor", Value(ObjectPtr(f)), 1);
        geom->set_own(name, Value(ObjectPtr(f)), 1);
        return f;
    };
    auto point_proto = vm.new_object();
    auto make_point = [point_proto](double x, double y) {
        auto o = std::make_shared<Object>(point_proto);
        o->set_own("x", Value(x));
        o->set_own("y", Value(y));
        return o;
    };
    auto point_ctor = geom_class("Point", point_proto, [make_point](VM& vm, CallArgs& a) -> ObjectPtr {
        return make_point(a[0].undefined() ? 0 : vm.to_number(a[0]), a[1].undefined() ? 0 : vm.to_number(a[1]));
    });
    auto px = [](VM& vm, const Value& p) { return vm.to_number(vm.get_member(p, "x")); };
    auto py = [](VM& vm, const Value& p) { return vm.to_number(vm.get_member(p, "y")); };
    v.method(point_proto, "add", [=](VM& vm, CallArgs& a) { return Value(ObjectPtr(make_point(px(vm, a.self) + px(vm, a[0]), py(vm, a.self) + py(vm, a[0])))); });
    v.method(point_proto, "subtract", [=](VM& vm, CallArgs& a) { return Value(ObjectPtr(make_point(px(vm, a.self) - px(vm, a[0]), py(vm, a.self) - py(vm, a[0])))); });
    v.method(point_proto, "clone", [=](VM& vm, CallArgs& a) { return Value(ObjectPtr(make_point(px(vm, a.self), py(vm, a.self)))); });
    v.method(point_proto, "offset", [=](VM& vm, CallArgs& a) {
        vm.set_member(a.self, "x", Value(px(vm, a.self) + vm.to_number(a[0])));
        vm.set_member(a.self, "y", Value(py(vm, a.self) + vm.to_number(a[1])));
        return Value();
    });
    v.method(point_proto, "normalize", [=](VM& vm, CallArgs& a) {
        const double x = px(vm, a.self), y = py(vm, a.self), l = std::sqrt(x * x + y * y), k = l > 0 ? vm.to_number(a[0]) / l : 0;
        vm.set_member(a.self, "x", Value(x * k));
        vm.set_member(a.self, "y", Value(y * k));
        return Value();
    });
    v.method(point_proto, "equals", [=](VM& vm, CallArgs& a) { return Value(px(vm, a.self) == px(vm, a[0]) && py(vm, a.self) == py(vm, a[0])); });
    v.method(point_proto, "toString", [=](VM& vm, CallArgs& a) {
        return Value("(x=" + VM::number_to_string(px(vm, a.self)) + ", y=" + VM::number_to_string(py(vm, a.self)) + ")");
    });
    {
        auto getter = vm.native([=](VM& vm, CallArgs& a) { return Value(std::hypot(px(vm, a.self), py(vm, a.self))); });
        point_proto->own("length").getter = getter;
    }
    v.method(ObjectPtr(point_ctor), "distance", [=](VM& vm, CallArgs& a) {
        return Value(std::hypot(px(vm, a[0]) - px(vm, a[1]), py(vm, a[0]) - py(vm, a[1])));
    });
    v.method(ObjectPtr(point_ctor), "interpolate", [=](VM& vm, CallArgs& a) {
        const double f = vm.to_number(a[2]);
        return Value(ObjectPtr(make_point(px(vm, a[1]) + (px(vm, a[0]) - px(vm, a[1])) * f, py(vm, a[1]) + (py(vm, a[0]) - py(vm, a[1])) * f)));
    });
    v.method(ObjectPtr(point_ctor), "polar", [=](VM& vm, CallArgs& a) {
        const double l = vm.to_number(a[0]), t = vm.to_number(a[1]);
        return Value(ObjectPtr(make_point(l * std::cos(t), l * std::sin(t))));
    });

    auto ct_proto = vm.new_object();
    static const char* const kCt[] = {"redMultiplier", "greenMultiplier", "blueMultiplier", "alphaMultiplier",
                                      "redOffset", "greenOffset", "blueOffset", "alphaOffset"};
    auto make_ct = [ct_proto](const CxForm& cx) {
        auto o = std::make_shared<Object>(ct_proto);
        for (int k = 0; k < 4; ++k) {
            o->set_own(kCt[k], Value(double(cx.mul[k])));
            o->set_own(kCt[k + 4], Value(double(cx.add[k] * 255)));
        }
        return o;
    };
    auto read_ct = [](VM& vm, const Value& o) {
        CxForm cx;
        for (int k = 0; k < 4; ++k) {
            const Value m = vm.get_member(o, kCt[k]), a = vm.get_member(o, kCt[k + 4]);
            cx.mul[k] = m.undefined() ? 1.0f : float(vm.to_number(m));
            cx.add[k] = a.undefined() ? 0.0f : float(vm.to_number(a) / 255);
        }
        return cx;
    };
    geom_class("ColorTransform", ct_proto, [ct_proto](VM& vm, CallArgs& a) -> ObjectPtr {
        auto o = std::make_shared<Object>(ct_proto);
        for (int k = 0; k < 8; ++k) o->set_own(kCt[k], Value(a[size_t(k)].undefined() ? (k < 4 ? 1.0 : 0.0) : vm.to_number(a[size_t(k)])));
        return o;
    });
    {
        Property& rgb = ct_proto->own("rgb");
        rgb.getter = vm.native([](VM& vm, CallArgs& a) {
            const int r = int(vm.to_number(vm.get_member(a.self, "redOffset"))), g = int(vm.to_number(vm.get_member(a.self, "greenOffset"))),
                      b = int(vm.to_number(vm.get_member(a.self, "blueOffset")));
            return Value(double((r & 255) << 16 | (g & 255) << 8 | (b & 255)));
        });
        rgb.setter = vm.native([](VM& vm, CallArgs& a) {
            const uint32_t c = uint32_t(vm.to_number(a[0]));
            for (int k = 0; k < 3; ++k) vm.set_member(a.self, kCt[k], Value(0.0));
            vm.set_member(a.self, "redOffset", Value(double(c >> 16 & 255)));
            vm.set_member(a.self, "greenOffset", Value(double(c >> 8 & 255)));
            vm.set_member(a.self, "blueOffset", Value(double(c & 255)));
            return Value();
        });
    }
    auto tr_proto = vm.new_object();
    auto tr_ctor = geom_class("Transform", tr_proto, [tr_proto](VM&, CallArgs& a) -> ObjectPtr {
        auto o = std::make_shared<Object>(tr_proto);
        o->set_own("_clip", a[0], 1);
        return o;
    });
    {
        Property& ctp = tr_proto->own("colorTransform");
        ctp.getter = vm.native([make_ct](VM& vm, CallArgs& a) {
            Instance* i = vm.get_member(a.self, "_clip").instance();
            return Value(ObjectPtr(make_ct(i ? i->cxform : CxForm{})));
        });
        ctp.setter = vm.native([read_ct](VM& vm, CallArgs& a) {
            if (Instance* i = vm.get_member(a.self, "_clip").instance()) {
                i->cxform = read_ct(vm, a[0]);
                i->accept_moves = false;
            }
            return Value();
        });
        Property& cct = tr_proto->own("concatenatedColorTransform");
        cct.getter = vm.native([make_ct](VM& vm, CallArgs& a) {
            Instance* i = vm.get_member(a.self, "_clip").instance();
            return Value(ObjectPtr(make_ct(i ? i->world_cxform() : CxForm{})));
        });
        Property& tp = mc->own("transform");
        tp.getter = vm.native([tr_ctor](VM& vm, CallArgs& a) { return vm.construct(Value(ObjectPtr(tr_ctor)), {a.self}); });
        tp.setter = vm.native([read_ct](VM& vm, CallArgs& a) {
            Instance* i = a.self.instance();
            const Value ct = vm.get_member(a[0], "colorTransform");
            if (i && ct.is_object()) i->cxform = read_ct(vm, ct);
            return Value();
        });
    }

    // --- the game's natives (CMenuManager::Init's registerNativeFunction table)
    vm.global->set_own("$version", Value("gameSWF"));
    auto forward = [this](const char* name, NativeFunction::Fn fallback) {
        std::string n = name;
        vm.method(vm.global, n, [this, n, fallback](VM& vm, CallArgs& a) {
            if (on_native) {
                Value r = on_native(n, a);
                if (!r.undefined()) return r;
            }
            return fallback ? fallback(vm, a) : Value();
        });
    };
    auto get_string = [](VM& vm, CallArgs& a) {
        const std::string id = vm.to_string(a[0]);
        return Value(vm.get_string ? vm.get_string(id) : id);
    };
    forward("getString", get_string);
    forward("NativeGetString", get_string);
    forward("playSound", [](VM& vm, CallArgs& a) {
        if (vm.play_sound) vm.play_sound(vm.to_string(a[0]));
        return Value();
    });
    forward("getCurrentLanguage", [](VM&, CallArgs&) { return Value("en"); });
    forward("getRealScreenWidth", [this](VM&, CallArgs&) { return Value(double(screen_.x)); });
    forward("getRealScreenHeight", [this](VM&, CallArgs&) { return Value(double(screen_.y)); });
    forward("getRealScreenRatio", [this](VM&, CallArgs&) { return Value(double(screen_.x / std::max(screen_.y, 1.0f))); });
    forward("getScreenConfig", [](VM& vm, CallArgs&) {
        auto o = vm.new_object();
        o->set_own("_bScaleX", Value(false));
        o->set_own("_bScaleY", Value(false));
        o->set_own("_inverseScale", Value(1.0));
        o->set_own("_scale", Value(1.0));
        return Value(o);
    });
    forward("canStartNewGame", [](VM&, CallArgs&) { return Value(true); });
    forward("canRestartLevel", [](VM&, CallArgs&) { return Value(true); });
    forward("getConvertedValue", [](VM&, CallArgs& a) { return a[0]; });
    forward("getGameParamValue", [](VM&, CallArgs&) { return Value(0); });
    for (const char* n : {"NativePushMenu", "NativeSwitchMenu", "NativePopMenu", "NativeStartLevel", "registerInGameEvent",
                          "registerCarEvent", "registerInGameEventExtended", "registerPressSceneButton", "loadNextStory",
                          "registerInGameParamValue", "registerShopEvent", "showShopPopup", "registerHUDEvent"})
        forward(n, nullptr);
}

}  // namespace tdkr::swf
