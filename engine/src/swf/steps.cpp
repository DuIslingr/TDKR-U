#include "swf/steps.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace tdkr::swf {

namespace {

void print_tree_impl(Instance* i, int indent, int max_depth) {
    std::printf("%*s%s [%s d=%d ch=%u f=%d/%d%s%s] x=%.1f y=%.1f a=%.2f\n", indent * 2, "", i->name.c_str(),
                i->kind == Instance::Clip ? "clip" : i->kind == Instance::Shape ? "shape" : "text", i->depth,
                i->character, i->frame + 1, i->total_frames(), i->visible ? "" : " hidden", i->playing ? " playing" : "",
                i->matrix.tx / 20, i->matrix.ty / 20, i->cxform.mul.a);
    if (i->kind == Instance::Text && !i->text.empty()) std::printf("%*s  \"%s\"\n", indent * 2, "", i->text.c_str());
    if (indent >= max_depth) return;
    for (auto& c : i->children)
        if (c->kind != Instance::Shape) print_tree_impl(c.get(), indent + 1, max_depth);
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t p = 0;
    for (;;) {
        const size_t q = s.find(sep, p);
        out.push_back(s.substr(p, q == std::string::npos ? std::string::npos : q - p));
        if (q == std::string::npos) return out;
        p = q + 1;
    }
}

Value parse_value(const std::string& v) {
    char* end = nullptr;
    const double d = std::strtod(v.c_str(), &end);
    if (end && !*end && !v.empty()) return Value(d);
    if (v == "true") return Value(true);
    if (v == "false") return Value(false);
    return Value(v);
}

}  // namespace

void print_tree(Instance* inst, int depth, int max_depth) { print_tree_impl(inst, depth, max_depth); }

bool run_step(Player& player, const std::string& s, std::string* error) {
    const auto eq = s.find('=');
    const std::string kind = s.substr(0, eq), rest = eq == std::string::npos ? "" : s.substr(eq + 1);
    const auto p = split(rest, ':');
    auto arg = [&](size_t k) { return k < p.size() ? p[k] : std::string(); };
    if (kind == "goto") player.goto_and_play(arg(0), arg(1));
    else if (kind == "stop") player.goto_and_stop(arg(0), arg(1));
    else if (kind == "hide") player.set_visible(arg(0), false);
    else if (kind == "show") player.set_visible(arg(0), true);
    else if (kind == "text") player.set_text(arg(0), arg(1));
    else if (kind == "load") player.load_movie(arg(0), arg(1));
    else if (kind == "frames") {
        for (int k = 0, n = std::atoi(arg(0).c_str()); k < n; ++k) player.step_frame();
    } else if (kind == "set") {
        player.set_member(arg(0), arg(1), parse_value(arg(2)));
    } else if (kind == "call") {
        std::vector<Value> args;
        for (size_t k = 2; k < p.size(); ++k) args.push_back(parse_value(p[k]));
        const Value r = player.invoke(arg(0), arg(1), args);
        std::printf("call %s.%s -> %s\n", arg(0).c_str(), arg(1).c_str(), player.vm.to_string(r).c_str());
    } else if (kind == "tree") {
        Instance* i = player.find(arg(0));
        if (i) print_tree(i, 0, arg(1).empty() ? 3 : std::atoi(arg(1).c_str()));
        else std::printf("tree: no %s\n", arg(0).c_str());
    } else {
        if (error) *error = "unknown step " + s;
        return false;
    }
    return true;
}

}  // namespace tdkr::swf
