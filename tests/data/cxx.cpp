// c++ built twice like kernels.c: exceptions through the guest's own unwinder, rtti, libc++

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef NATIVE
#define K(name) native_##name
#else
#define K(name) name
#endif

namespace {

struct Shape {
    virtual ~Shape() = default;
    virtual double area() const = 0;
};

struct Square : Shape {
    double side;
    explicit Square(double s) : side(s) {}
    double area() const override { return side * side; }
};

struct Circle : Shape {
    double r;
    explicit Circle(double radius) : r(radius) {}
    double area() const override { return 3.0 * r * r; }
};

// a string per frame so unwinding has destructors to run
[[gnu::noinline]] int deep_throw(int depth) {
    if (depth == 0) {
        throw std::runtime_error("bottom");
    }
    std::string keep = "frame " + std::to_string(depth);
    return deep_throw(depth - 1) + int(keep.size());
}

int destroyed = 0;

struct Counted {
    ~Counted() { ++destroyed; }
};

[[gnu::noinline]] void throws_int(int v) {
    Counted c;
    throw v;
}

}  // namespace

extern "C" {

int K(cxx_catch)(int depth) {
    try {
        return deep_throw(depth);
    } catch (const std::runtime_error& e) {
        return 1000 + int(std::string(e.what()).size());
    }
}

int K(cxx_destructors)(int n) {
    destroyed = 0;
    int caught = 0;
    for (int i = 0; i < n; ++i) {
        try {
            throws_int(i);
        } catch (int v) {
            caught += v;
        }
    }
    return caught * 100 + destroyed;
}

int K(cxx_rethrow)() {
    int r = 0;
    try {
        try {
            throw std::out_of_range("x");
        } catch (const std::logic_error&) {
            r += 1;
            throw;
        }
    } catch (const std::exception&) {
        r += 10;
    }
    try {
        throw 3.5;
    } catch (...) {
        r += 100;
    }
    return r;
}

double K(cxx_virtual)(int n) {
    std::vector<std::unique_ptr<Shape>> shapes;
    for (int i = 0; i < n; ++i) {
        if (i % 2) {
            shapes.push_back(std::make_unique<Square>(i));
        } else {
            shapes.push_back(std::make_unique<Circle>(i));
        }
    }
    double total = 0;
    for (auto& s : shapes) {
        total += s->area();
        if (dynamic_cast<Square*>(s.get())) {
            total += 0.5;
        }
    }
    return total;
}

int K(cxx_containers)(int n, char* out, int size) {
    std::map<std::string, int> m;
    for (int i = 0; i < n; ++i) {
        m[std::to_string(i * 7 % n)] += i;
    }
    std::vector<int> v;
    for (auto& [k, x] : m) {
        v.push_back(x + int(k.size()));
    }
    std::sort(v.begin(), v.end(), std::greater<>());
    std::ostringstream text;
    text << v.front() << ":" << v.back() << ":" << v.size() << ":" << 1.25 * n;
    std::string s = text.str();
    std::snprintf(out, size_t(size), "%s", s.c_str());
    return int(s.size());
}

int K(cxx_vector_at)(int i) {
    std::vector<int> v(3, 7);
    try {
        return v.at(size_t(i));
    } catch (const std::out_of_range&) {
        return -1;
    }
}

}
