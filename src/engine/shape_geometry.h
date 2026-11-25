// SPDX-License-Identifier: Apache-2.0
// shape_geometry.h - the outlines of Excel's preset shapes (Insert > Shapes), drawn the same way on screen and on paper
//
// A shape is described by Office's msoAutoShapeType number (msoShapeRectangle 1, msoShapeOval 9, msoShapeRightArrow
// 33...). Its drawing is a list of simple polygons to fill (no holes: a donut is cut into pieces) and of lines to
// stroke, in a w x h box with y down; the drawer maps them to the screen or the page, then flips and rotates them.
#pragma once

#include <string>
#include <vector>

namespace yc::model {
struct ShapeObj;
}

namespace yc::engine {

struct ShapePt {
  double x = 0, y = 0;
};

struct ShapeGeom {
  struct Fill {
    std::vector<ShapePt> pts;
    int shade = 0;  // a face drawn darker (-1, -2) or lighter (1) than the fill, as Excel's cube, can and bevel
  };
  struct Stroke {
    std::vector<ShapePt> pts;
    bool closed = true;
  };
  std::vector<Fill> fills;
  std::vector<Stroke> strokes;
};

// the drawing of preset shape type in a w x h box (adj: its adjustments, nullptr: Excel's defaults); false for a type
// that isn't drawn this way
bool AutoShapeGeometry(int type, double w, double h, ShapeGeom* g, const std::vector<double>* adj = nullptr);

// Shape.Adjustments: Excel's default values of a preset's adjustments (empty: it has none), where its yellow handles are
// in a w x h box, and the adjustments after handle k is dragged to p (box coordinates)
std::vector<double> AutoShapeAdjustDefaults(int type);
std::vector<ShapePt> AutoShapeHandles(int type, double w, double h, const std::vector<double>& adj);
bool AutoShapeDragHandle(int type, size_t handle, double w, double h, ShapePt p, std::vector<double>* adj);

// one entry of Excel's Insert > Shapes gallery: type is the msoAutoShapeType, or for the Lines group a negative code
// (kLine...); group is the gallery's heading
struct ShapeGalleryEntry {
  int type;
  const char* name;   // Excel's name, the start of a new shape's name ("Arrow: Right 4")
  const char* label;  // the name shown (translated)
  const char* group;  // "Lines", "Rectangles", "Basic Shapes", "Block Arrows", "Equation Shapes", "Flowchart",
                      // "Stars and Banners", "Callouts"
};
const std::vector<ShapeGalleryEntry>& ShapeGallery();
// the gallery's name of preset shape type ("Oval", "Arrow: Right"), nullptr for an unknown one
const char* AutoShapeName(int type);

// the Lines group (not AutoShapes in Excel: straight, elbow and curved connectors, curves and freeforms)
enum ShapeLineCode : int {
  kLine = -1,
  kLineArrow = -2,
  kLineDoubleArrow = -3,
  kElbow = -4,
  kElbowArrow = -5,
  kElbowDoubleArrow = -6,
  kCurved = -7,
  kCurvedArrow = -8,
  kCurvedDoubleArrow = -9,
  kCurve = -10,
  kFreeform = -11,
  kScribble = -12,
};

// msoShapeNotPrimitive: a freeform (its own points)
constexpr int kFreeformType = 138;

// a connector's path from a to b: 1 straight, 2 elbow (msoConnectorElbow), 3 curved (msoConnectorCurve)
std::vector<ShapePt> ConnectorPath(int connector, ShapePt a, ShapePt b);

// a freeform's points (x0, y0, x1, y1... as fractions of its box) in a w x h box; smooth: a curve through them
std::vector<ShapePt> FreeformPath(const std::vector<double>& pts, bool smooth, double w, double h);

// the drawing of a shape that isn't drawn by its kind: a preset shape (auto_type) or a freeform (path); false otherwise
bool ShapeObjGeometry(const model::ShapeObj& sh, double w, double h, ShapeGeom* g);

// whether point p (box coordinates) is inside the shape's filled area
bool InShapeGeometry(const ShapeGeom& g, ShapePt p);

}  // namespace yc::engine
