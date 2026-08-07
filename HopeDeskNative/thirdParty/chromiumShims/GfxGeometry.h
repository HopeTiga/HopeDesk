#pragma once

// Minimal gfx::Size / gfx::Rect for the AV1 DXVA port.
#include <string>

namespace gfx {

class Size {
public:
    Size() : width_(0), height_(0) {}
    Size(int w, int h) : width_(w), height_(h) {}

    int width() const { return width_; }
    int height() const { return height_; }
    bool IsEmpty() const { return width_ <= 0 || height_ <= 0; }
    bool operator==(const Size& o) const {
        return width_ == o.width_ && height_ == o.height_;
    }
    bool operator!=(const Size& o) const { return !(*this == o); }
    std::string ToString() const { return std::string(); }

private:
    int width_;
    int height_;
};

class Rect {
public:
    Rect() : x_(0), y_(0), width_(0), height_(0) {}
    // (width, height) at origin (0,0).
    Rect(int w, int h) : x_(0), y_(0), width_(w), height_(h) {}
    Rect(int x, int y, int w, int h) : x_(x), y_(y), width_(w), height_(h) {}
    Rect(const Size& s) : x_(0), y_(0), width_(s.width()), height_(s.height()) {}

    int x() const { return x_; }
    int y() const { return y_; }
    int width() const { return width_; }
    int height() const { return height_; }
    bool IsEmpty() const { return width_ <= 0 || height_ <= 0; }
    bool Contains(const Rect& r) const {
        return r.x_ >= x_ && r.y_ >= y_ &&
               r.x_ + r.width_ <= x_ + width_ &&
               r.y_ + r.height_ <= y_ + height_;
    }
    bool operator==(const Rect& o) const {
        return x_ == o.x_ && y_ == o.y_ && width_ == o.width_ && height_ == o.height_;
    }
    bool operator!=(const Rect& o) const { return !(*this == o); }
    std::string ToString() const { return std::string(); }

private:
    int x_;
    int y_;
    int width_;
    int height_;
};

} // namespace gfx
