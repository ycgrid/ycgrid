// SPDX-License-Identifier: Apache-2.0
// Every reference the model keeps outside cell formulas, against every operation that moves, renames, breaks or
// copies references. Each kind of text (model/ref_texts.h) and each rectangle kept for cells is a slot here; a slot
// gets every operation, so a kind that one operation forgets fails here instead of in a user's workbook. Five places
// kept lists of their own before: a copied sheet's Solver model named the source, an icon set's formula thresholds
// stayed put on insert, a color scale's formulas kept a deleted sheet's name, a scatter chart's X error bars and the
// ranges a protected sheet lets people edit never followed their cells.
// Also: the write policy (engine::PolicyFor), one row per source.
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

#include "model/ref_texts.h"

#include <functional>
#include <string>
#include <vector>

namespace model = yc::model;
namespace engine = yc::engine;
using yc::core::Rect;

namespace {

// a slot: where a reference is kept, how a test puts one there and reads it back
struct Slot {
  const char* name;
  bool rule;  // a rule's text: on a copied sheet it names no sheet (=$B$3), as Excel's rules do
  bool cell;  // a text linked to one cell ("=Data!$B$3"); otherwise a range
  std::function<void(engine::Document&, uint32_t host, const std::string& text)> put;
  std::function<std::string(const engine::Document&, uint32_t host)> get;
};

model::Sheet& Mut(engine::Document& d, uint32_t id) { return *d.wb_mut().MutSheetById(id); }

// texts kept with "=" in front: linked texts, a list's range, names
std::string Eq(const std::string& t) { return "=" + t; }

std::vector<Slot> TextSlots() {
  std::vector<Slot> v;
  // a chart with one series and one formatted point, every text that may name cells
  auto chart = [&](const char* name, bool cell, std::function<std::string&(model::ChartObj&)> at) {
    v.push_back(Slot{name, false, cell,
                     [at, cell](engine::Document& d, uint32_t host, const std::string& t) {
                       auto& charts = Mut(d, host).charts.mut();
                       if (charts.empty()) {
                         model::ChartObj c;
                         c.series.emplace_back();
                         c.series[0].points.emplace_back();
                         charts.push_back(c);
                       }
                       at(charts[0]) = cell ? Eq(t) : t;
                     },
                     [at](const engine::Document& d, uint32_t host) {
                       model::ChartObj c = d.S(host)->charts->at(0);
                       return at(c);
                     }});
  };
  chart("chart.source", false, [](model::ChartObj& c) -> std::string& { return c.source; });
  chart("chart.title", true, [](model::ChartObj& c) -> std::string& { return c.title; });
  chart("chart.cat_axis.title", true, [](model::ChartObj& c) -> std::string& { return c.cat_axis.title; });
  chart("chart.val_axis.title", true, [](model::ChartObj& c) -> std::string& { return c.val_axis.title; });
  chart("chart.val_axis2.title", true, [](model::ChartObj& c) -> std::string& { return c.val_axis2.title; });
  chart("series.name", true, [](model::ChartObj& c) -> std::string& { return c.series[0].name; });
  chart("series.values", false, [](model::ChartObj& c) -> std::string& { return c.series[0].values; });
  chart("series.x_values", false, [](model::ChartObj& c) -> std::string& { return c.series[0].x_values; });
  chart("series.err_plus", false, [](model::ChartObj& c) -> std::string& { return c.series[0].err_plus; });
  chart("series.err_minus", false, [](model::ChartObj& c) -> std::string& { return c.series[0].err_minus; });
  chart("series.xerr_plus", false, [](model::ChartObj& c) -> std::string& { return c.series[0].xerr_plus; });
  chart("series.xerr_minus", false, [](model::ChartObj& c) -> std::string& { return c.series[0].xerr_minus; });
  chart("series.label_range", false, [](model::ChartObj& c) -> std::string& { return c.series[0].label_range; });
  chart("series.bubble_sizes", false, [](model::ChartObj& c) -> std::string& { return c.series[0].bubble_sizes; });
  chart("point.label_text", true, [](model::ChartObj& c) -> std::string& { return c.series[0].points[0].label_text; });

  auto control = [&](const char* name, bool cell, std::function<std::string&(model::Control&)> at) {
    v.push_back(Slot{name, false, cell,
                     [at](engine::Document& d, uint32_t host, const std::string& t) {
                       auto& cs = Mut(d, host).controls.mut();
                       if (cs.empty()) cs.emplace_back();
                       at(cs[0]) = t;
                     },
                     [at](const engine::Document& d, uint32_t host) {
                       model::Control c = d.S(host)->controls->at(0);
                       return at(c);
                     }});
  };
  control("control.linked_cell", true, [](model::Control& c) -> std::string& { return c.linked_cell; });
  control("control.list_fill_range", false, [](model::Control& c) -> std::string& { return c.list_fill_range; });

  v.push_back(Slot{"sparklines.date_range", false, false,
                   [](engine::Document& d, uint32_t host, const std::string& t) {
                     model::SparklineGroup g;
                     g.items.push_back(model::Sparkline{0, 7, host, Rect{1, 7, 2, 7}});  // H1 from H2:H3 of its own sheet
                     g.date_range = t;
                     Mut(d, host).sparklines.mut().push_back(g);
                   },
                   [](const engine::Document& d, uint32_t host) { return d.S(host)->sparklines->at(0).date_range; }});

  auto solver = [&](const char* name, bool cell, std::function<std::string&(model::SolverModel&)> at) {
    v.push_back(Slot{name, false, cell,
                     [at](engine::Document& d, uint32_t host, const std::string& t) {
                       model::SolverModel& m = Mut(d, host).solver.mut();
                       m.defined = true;
                       if (m.constraints.empty()) m.constraints.push_back(model::SolverConstraint{});
                       at(m) = t;
                     },
                     [at](const engine::Document& d, uint32_t host) {
                       model::SolverModel m = *d.S(host)->solver;
                       return at(m);
                     }});
  };
  solver("solver.objective", true, [](model::SolverModel& m) -> std::string& { return m.objective; });
  solver("solver.changing", false, [](model::SolverModel& m) -> std::string& { return m.changing; });
  solver("solver.constraint.ref", false, [](model::SolverModel& m) -> std::string& { return m.constraints[0].ref; });
  solver("solver.constraint.rhs", true, [](model::SolverModel& m) -> std::string& { return m.constraints[0].rhs; });

  // conditional formats, applied to F1:F2 of the host; their formulas are kept without '='
  auto cf = [&](const char* name, std::function<std::string&(model::CfRule&)> at) {
    v.push_back(Slot{name, true, false,
                     [at](engine::Document& d, uint32_t host, const std::string& t) {
                       auto& rules = Mut(d, host).cond_formats.mut();
                       if (rules.empty()) {
                         model::CfRule r;
                         r.ranges.push_back(Rect{0, 5, 1, 5});
                         r.icon_thresholds.emplace_back();
                         rules.push_back(r);
                       }
                       at(rules[0]) = "SUM(" + t + ")";
                     },
                     [at](const engine::Document& d, uint32_t host) {
                       model::CfRule r = d.S(host)->cond_formats->at(0);
                       return at(r);
                     }});
  };
  cf("cf.formula1", [](model::CfRule& r) -> std::string& { return r.formula1; });
  cf("cf.formula2", [](model::CfRule& r) -> std::string& { return r.formula2; });
  cf("cf.point_formula[0]", [](model::CfRule& r) -> std::string& { return r.point_formula[0]; });
  cf("cf.point_formula[2]", [](model::CfRule& r) -> std::string& { return r.point_formula[2]; });
  cf("cf.bar_min_formula", [](model::CfRule& r) -> std::string& { return r.bar_min_formula; });
  cf("cf.bar_max_formula", [](model::CfRule& r) -> std::string& { return r.bar_max_formula; });
  cf("cf.icon_thresholds.formula", [](model::CfRule& r) -> std::string& { return r.icon_thresholds[0].formula; });

  auto dv = [&](const char* name, model::DvType type, std::function<std::string&(model::DvRule&)> at) {
    v.push_back(Slot{name, true, false,
                     [at, type](engine::Document& d, uint32_t host, const std::string& t) {
                       model::DvRule r;
                       r.type = type;
                       r.ranges.push_back(Rect{0, 6, 1, 6});
                       at(r) = Eq(t);
                       Mut(d, host).validations.mut().push_back(r);
                     },
                     [at](const engine::Document& d, uint32_t host) {
                       model::DvRule r = d.S(host)->validations->at(0);
                       return at(r);
                     }});
  };
  dv("dv.formula1 (list)", model::DvType::List, [](model::DvRule& r) -> std::string& { return r.formula1; });
  dv("dv.formula2", model::DvType::Decimal, [](model::DvRule& r) -> std::string& { return r.formula2; });

  auto panel = [&](const char* name, bool cell, std::function<std::string&(model::PanelControl&)> at) {
    v.push_back(Slot{name, false, cell,
                     [at](engine::Document& d, uint32_t, const std::string& t) {
                       auto& ps = d.wb_mut().panels.mut();
                       if (ps.empty()) {
                         ps.emplace_back();
                         ps[0].name = "P";
                         ps[0].controls.emplace_back();
                       }
                       at(ps[0].controls[0]) = t;
                     },
                     [at](const engine::Document& d, uint32_t) {
                       model::PanelControl c = d.wb().panels->at(0).controls.at(0);
                       return at(c);
                     }});
  };
  panel("panel.control.linked_cell", true, [](model::PanelControl& c) -> std::string& { return c.linked_cell; });
  panel("panel.control.list_fill_range", false, [](model::PanelControl& c) -> std::string& { return c.list_fill_range; });

  v.push_back(Slot{"name.refers_to", false, false,
                   [](engine::Document& d, uint32_t, const std::string& t) {
                     model::NameDef n;
                     n.name = "Kept";
                     n.refers_to = Eq(t);
                     d.wb_mut().names.mut().push_back(n);
                   },
                   [](const engine::Document& d, uint32_t) {
                     for (const auto& n : *d.wb().names) {
                       if (n.name == "Kept") return n.refers_to;
                     }
                     return std::string("(gone)");
                   }});
  return v;
}

bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// an operation on the sheet Data and what a reference to Data!$B$3:$B$5 (or Data!$B$3) reads after it
struct Op {
  const char* name;
  std::function<void(engine::Document&, uint32_t data)> run;
  const char* range;  // the part a range reads after it
  const char* cell;   // the part a one-cell text reads
};

const std::vector<Op>& Ops() {
  static const std::vector<Op> ops = {
      {"insert two rows above", [](engine::Document& d, uint32_t data) { d.InsertRows(data, 0, 2); }, "$B$5:$B$7", "$B$5"},
      {"delete the first row", [](engine::Document& d, uint32_t data) { d.DeleteRows(data, 0, 1); }, "$B$2:$B$4", "$B$2"},
      {"insert a column at A", [](engine::Document& d, uint32_t data) { d.InsertCols(data, 0, 1); }, "$C$3:$C$5", "$C$3"},
      {"insert cells shifting B down", [](engine::Document& d, uint32_t data) { d.InsertCells(data, Rect{0, 1, 0, 1}, true); }, "$B$4:$B$6", "$B$4"},
      {"cut B3:B5 to D3:D5", [](engine::Document& d, uint32_t data) { REQUIRE(d.MoveCells(data, Rect{2, 1, 4, 1}, data, 2, 3)); }, "$D$3:$D$5", "$D$3"},
      {"rename Data to Prices", [](engine::Document& d, uint32_t data) { REQUIRE(d.RenameSheet(data, "Prices")); }, "Prices!", "Prices!"},
      {"delete the sheet Data", [](engine::Document& d, uint32_t data) { REQUIRE(d.DeleteSheet(data)); }, "#REF!", "#REF!"},
  };
  return ops;
}

}  // namespace

TEST_CASE("Every reference kept as text follows every operation that moves, renames or breaks it") {
  for (const Slot& slot : TextSlots()) {
    for (const Op& op : Ops()) {
      yctest::Doc t;
      const uint32_t data = t.d.AddSheet("Data");
      const uint32_t host = t.d.AddSheet("Host");
      slot.put(t.d, host, slot.cell ? "Data!$B$3" : "Data!$B$3:$B$5");
      op.run(t.d, data);
      const std::string got = slot.get(t.d, host);
      INFO(slot.name << " after " << op.name << ": " << got);
      CHECK(Has(got, slot.cell ? op.cell : op.range));
    }
  }
}

TEST_CASE("A copied sheet's references to itself name the copy; its rules name no sheet") {
  for (const Slot& slot : TextSlots()) {
    // a workbook name and a panel stay the workbook's (copied names: test_engine_struct)
    if (std::string(slot.name) == "name.refers_to" || Has(slot.name, "panel.")) continue;
    yctest::Doc t;
    const uint32_t host = t.d.AddSheet("Host");
    slot.put(t.d, host, slot.cell ? "Host!$B$3" : "Host!$B$3:$B$5");
    const uint32_t copy = t.d.CopySheet(host);
    REQUIRE(copy != 0);
    const std::string got = slot.get(t.d, copy), source = slot.get(t.d, host);
    INFO(slot.name << ": the copy reads " << got << ", the source " << source);
    CHECK(Has(source, "Host!"));
    if (slot.rule) {
      CHECK(!Has(got, "Host"));
      CHECK(Has(got, slot.cell ? "$B$3" : "$B$3:$B$5"));
    } else {
      CHECK(Has(got, "'Host (2)'!"));
    }
  }
}

namespace {

// what keeps cells as a rectangle (or one cell) on the sheet Data; read back as A1 text, "" once it is gone
struct RectSlot {
  const char* name;
  // when all its cells are cut and pasted: 1 it follows them, 0 it stays (Excel 16, measured), -1 not measured (a
  // data table: Excel refuses to cut part of one; a scenario: the probe could not add one through COM)
  int cut;
  std::function<void(engine::Document&, uint32_t data, uint32_t host, const Rect&)> put;
  std::function<std::string(const engine::Document&, uint32_t data, uint32_t host)> get;
};

std::string A1(const Rect& r) { return yc::core::RectA1(r); }

std::vector<RectSlot> RectSlots() {
  std::vector<RectSlot> v;
  v.push_back(RectSlot{"protection.ranges (Allow Edit Ranges)", true,
                       [](engine::Document& d, uint32_t data, uint32_t, const Rect& r) {
                         model::AllowEditRange e;
                         e.title = "R1";
                         e.rect = r;
                         Mut(d, data).protection.mut().ranges.push_back(e);
                       },
                       [](const engine::Document& d, uint32_t data, uint32_t) {
                         const auto& v = d.S(data)->protection->ranges;
                         return v.empty() ? std::string() : A1(v[0].rect);
                       }});
  v.push_back(RectSlot{"ignored_errors", false,
                       [](engine::Document& d, uint32_t data, uint32_t, const Rect& r) { Mut(d, data).ignored_errors.mut().push_back(r); },
                       [](const engine::Document& d, uint32_t data, uint32_t) {
                         const auto& v = *d.S(data)->ignored_errors;
                         return v.empty() ? std::string() : A1(v[0]);
                       }});
  v.push_back(RectSlot{"linked_pictures (on another sheet)", true,
                       [](engine::Document& d, uint32_t data, uint32_t host, const Rect& r) {
                         Mut(d, host).linked_pictures.mut().push_back(model::Sheet::LinkedPicture{1, data, r});
                       },
                       [](const engine::Document& d, uint32_t, uint32_t host) {
                         const auto& v = *d.S(host)->linked_pictures;
                         return v.empty() ? std::string() : A1(v[0].rect);
                       }});
  v.push_back(RectSlot{"page_setup.area (the print area)", true,
                       [](engine::Document& d, uint32_t data, uint32_t, const Rect& r) {
                         d.UpdatePageSetup(data, [&](model::PageSetup& ps) {
                           ps.has_area = true;
                           ps.area = r;
                         });
                       },
                       [](const engine::Document& d, uint32_t data, uint32_t) {
                         const auto& ps = *d.S(data)->page_setup;
                         return ps.has_area ? A1(ps.area) : std::string();
                       }});
  v.push_back(RectSlot{"page_setup.more_areas (the print area's second area)", true,
                       [](engine::Document& d, uint32_t data, uint32_t, const Rect& r) {
                         d.UpdatePageSetup(data, [&](model::PageSetup& ps) {
                           ps.has_area = true;
                           ps.area = Rect{30, 0, 31, 0};  // A31:A32, out of the way
                           ps.more_areas.push_back(r);
                         });
                       },
                       [](const engine::Document& d, uint32_t data, uint32_t) {
                         const auto& ps = *d.S(data)->page_setup;
                         return ps.more_areas.empty() ? std::string() : A1(ps.more_areas[0]);
                       }});
  v.push_back(RectSlot{"autofilter.range", 1,
                       [](engine::Document& d, uint32_t data, uint32_t, const Rect& r) {
                         auto& af = Mut(d, data).autofilter.mut();
                         af.on = true;
                         af.range = r;
                       },
                       [](const engine::Document& d, uint32_t data, uint32_t) {
                         const auto& af = *d.S(data)->autofilter;
                         return af.on ? A1(af.range) : std::string();
                       }});
  v.push_back(RectSlot{"autofilter.adv_range (an advanced filter's list)", 1,
                       [](engine::Document& d, uint32_t data, uint32_t, const Rect& r) { Mut(d, data).autofilter.mut().adv_range = r; },
                       [](const engine::Document& d, uint32_t data, uint32_t) {
                         const auto& af = *d.S(data)->autofilter;
                         return af.adv_range == Rect{} ? std::string() : A1(af.adv_range);
                       }});
  v.push_back(RectSlot{"cond_formats.ranges", 1,
                       [](engine::Document& d, uint32_t data, uint32_t, const Rect& r) {
                         model::CfRule rule;
                         rule.ranges.push_back(r);
                         rule.formula1 = "1";
                         Mut(d, data).cond_formats.mut().push_back(rule);
                       },
                       [](const engine::Document& d, uint32_t data, uint32_t) {
                         const auto& v = *d.S(data)->cond_formats;
                         return v.empty() || v[0].ranges.empty() ? std::string() : A1(v[0].ranges[0]);
                       }});
  v.push_back(RectSlot{"validations.ranges", 1,
                       [](engine::Document& d, uint32_t data, uint32_t, const Rect& r) {
                         model::DvRule rule;
                         rule.type = model::DvType::List;
                         rule.formula1 = "a,b";
                         rule.ranges.push_back(r);
                         Mut(d, data).validations.mut().push_back(rule);
                       },
                       [](const engine::Document& d, uint32_t data, uint32_t) {
                         const auto& v = *d.S(data)->validations;
                         return v.empty() || v[0].ranges.empty() ? std::string() : A1(v[0].ranges[0]);
                       }});
  v.push_back(RectSlot{"tables.range", 1,
                       [](engine::Document& d, uint32_t data, uint32_t, const Rect& r) {
                         model::Table tb;
                         tb.id = 900;
                         tb.name = "Kept";
                         tb.range = r;
                         tb.columns.push_back(model::TableColumn{"h", 0, ""});
                         Mut(d, data).tables.mut().push_back(tb);
                       },
                       [](const engine::Document& d, uint32_t data, uint32_t) {
                         const auto& v = *d.S(data)->tables;
                         return v.empty() ? std::string() : A1(v[0].range);
                       }});
  v.push_back(RectSlot{"data_tables.rect", -1,
                       [](engine::Document& d, uint32_t data, uint32_t, const Rect& r) {
                         model::DataTable dt;
                         dt.rect = r;
                         Mut(d, data).data_tables.mut().push_back(dt);
                       },
                       [](const engine::Document& d, uint32_t data, uint32_t) {
                         const auto& v = *d.S(data)->data_tables;
                         return v.empty() ? std::string() : A1(v[0].rect);
                       }});
  v.push_back(RectSlot{"sparklines.items.src (on another sheet)", 1,
                       [](engine::Document& d, uint32_t data, uint32_t host, const Rect& r) {
                         model::SparklineGroup g;
                         g.items.push_back(model::Sparkline{0, 0, data, r});
                         Mut(d, host).sparklines.mut().push_back(g);
                       },
                       [](const engine::Document& d, uint32_t, uint32_t host) {
                         const auto& v = *d.S(host)->sparklines;
                         return v.empty() || v[0].items.empty() ? std::string() : A1(v[0].items[0].src);
                       }});
  v.push_back(RectSlot{"scenarios.cells (its first cell)", -1,
                       [](engine::Document& d, uint32_t data, uint32_t, const Rect& r) {
                         model::Scenario sc;
                         sc.name = "S";
                         sc.cells.push_back(yc::core::CellPos{r.r0, r.c0});
                         sc.values.push_back(yc::core::Value::Number(1));
                         Mut(d, data).scenarios.mut().push_back(sc);
                       },
                       [](const engine::Document& d, uint32_t data, uint32_t) {
                         const auto& v = *d.S(data)->scenarios;
                         return v.empty() || v[0].cells.empty() ? std::string() : A1(Rect::Cell(v[0].cells[0].row, v[0].cells[0].col));
                       }});
  v.push_back(RectSlot{"pivots.src (a report on another sheet)", 1,
                       [](engine::Document& d, uint32_t data, uint32_t host, const Rect& r) {
                         model::PivotTable pt;
                         pt.id = 901;
                         pt.name = "PivotKept";
                         pt.src_sheet = data;
                         pt.src = r;
                         Mut(d, host).pivots.mut().push_back(pt);
                       },
                       [](const engine::Document& d, uint32_t, uint32_t host) {
                         const auto& v = *d.S(host)->pivots;
                         return v.empty() ? std::string() : A1(v[0].src);
                       }});
  v.push_back(RectSlot{"spills.rect (an array formula's cells)", 1,
                       [](engine::Document& d, uint32_t data, uint32_t, const Rect& r) {
                         REQUIRE(d.SetArrayFormula(data, r, "{1;2;3}"));  // Ctrl+Shift+Enter over the cells
                       },
                       [](const engine::Document& d, uint32_t data, uint32_t) {
                         const auto& v = *d.S(data)->spills;
                         return v.empty() ? std::string() : A1(v.begin()->second.rect);
                       }});
  v.push_back(RectSlot{"alerts", true,
                       [](engine::Document& d, uint32_t data, uint32_t, const Rect& r) {
                         model::AlertRule a;
                         a.sheet = data;
                         a.range = r;
                         d.wb_mut().alerts.mut().push_back(a);
                       },
                       [](const engine::Document& d, uint32_t, uint32_t) {
                         const auto& v = *d.wb().alerts;
                         return v.empty() ? std::string() : A1(v[0].range);
                       }});
  v.push_back(RectSlot{"parameters (its first cell)", true,
                       [](engine::Document& d, uint32_t data, uint32_t, const Rect& r) {
                         model::ParameterDef p;
                         p.sheet = data;
                         p.row = r.r0;
                         p.col = r.c0;
                         d.wb_mut().parameters.mut().push_back(p);
                       },
                       [](const engine::Document& d, uint32_t, uint32_t) {
                         const auto& v = *d.wb().parameters;
                         return v.empty() ? std::string() : A1(Rect::Cell(v[0].row, v[0].col));
                       }});
  return v;
}

}  // namespace

TEST_CASE("Every rectangle kept for cells moves with its cells, as in Excel, and goes when they are deleted") {
  const Rect b3b5{2, 1, 4, 1};
  struct Case {
    const char* op;
    std::function<void(engine::Document&, uint32_t)> run;
    const char* range;  // B3:B5 reads this after it
    const char* first;  // its first cell B3
  };
  const std::vector<Case> cases = {
      {"insert two rows above", [](engine::Document& d, uint32_t s) { d.InsertRows(s, 0, 2); }, "B5:B7", "B5"},
      {"delete the first row", [](engine::Document& d, uint32_t s) { d.DeleteRows(s, 0, 1); }, "B2:B4", "B2"},
      {"insert a column at A", [](engine::Document& d, uint32_t s) { d.InsertCols(s, 0, 1); }, "C3:C5", "C3"},
      {"delete rows 3:5", [](engine::Document& d, uint32_t s) { d.DeleteRows(s, 2, 3); }, "", ""},
  };
  for (const RectSlot& slot : RectSlots()) {
    for (const Case& c : cases) {
      // a report whose source rows are all deleted keeps its source as it was (Excel 16, measured: R7C8:R9C8 after
      // deleting rows 7:9)
      const bool keeps = Has(slot.name, "pivots.src") && *c.range == 0;
      yctest::Doc t;
      const uint32_t data = t.d.AddSheet("Data"), host = t.d.AddSheet("Host");
      slot.put(t.d, data, host, b3b5);
      c.run(t.d, data);
      const std::string got = slot.get(t.d, data, host);
      const bool one_cell = Has(slot.name, "first cell");
      INFO(slot.name << " after " << c.op << ": " << got);
      CHECK(got == (keeps ? std::string("B3:B5") : std::string(one_cell ? c.first : c.range)));
    }
    // cut and pasted with its cells
    yctest::Doc t;
    const uint32_t data = t.d.AddSheet("Data"), host = t.d.AddSheet("Host");
    slot.put(t.d, data, host, b3b5);
    INFO(slot.name);
    t.d.python_call = Has(slot.name, "parameters");  // a person can't cut a parameter cell; code can
    REQUIRE(t.d.MoveCells(data, b3b5, data, 4, 7));  // to H5:H7
    const std::string got = slot.get(t.d, data, host);
    const bool one_cell = Has(slot.name, "first cell");
    INFO(slot.name << " after a cut to H5:H7: " << got);
    if (slot.cut == 1) CHECK(got == (one_cell ? "H5" : "H5:H7"));
    if (Has(slot.name, "spills")) {  // still one array: its values come from the formula, none typed under it
      t.d.Recalc();
      CHECK(t.d.GetValue(data, 5, 7).AsDouble() == 2);
      CHECK(t.d.S(data)->GetFormula(5, 7) == 0);
    }
    else if (slot.cut == 0) CHECK(got == (one_cell ? "B3" : "B3:B5"));
  }
}

TEST_CASE("Write policy: one row per source decides what a write does besides storing the cell") {
  using engine::PolicyFor;
  // typing
  CHECK(PolicyFor(engine::kSrcUser).input);
  CHECK(PolicyFor(engine::kSrcUser).table_rules);
  CHECK(PolicyFor(engine::kSrcUser).infer_format);
  CHECK_FALSE(PolicyFor(engine::kSrcUser).foreign);
  // code
  CHECK(PolicyFor(engine::kSrcPython).table_rules);
  CHECK(PolicyFor(engine::kSrcPython).infer_format);
  CHECK(PolicyFor(engine::kSrcPython).foreign);
  // a file's cells: kept as the file saved them, no rules of typing
  CHECK(PolicyFor(engine::kSrcFile).input);
  CHECK_FALSE(PolicyFor(engine::kSrcFile).table_rules);
  CHECK_FALSE(PolicyFor(engine::kSrcFile).infer_format);
  CHECK(std::string(PolicyFor(engine::kSrcFile).name) == "file");
  // a calculation's own results
  CHECK_FALSE(PolicyFor(engine::kSrcCalc).input);
  CHECK_FALSE(PolicyFor(engine::kSrcCalc).table_rules);
  CHECK_FALSE(PolicyFor(engine::kSrcCalc | engine::kSrcNoEvents).input);
  CHECK_FALSE(PolicyFor(engine::kSrcUser | engine::kSrcCalc).infer_format);
  // live data and topics
  CHECK_FALSE(PolicyFor(engine::kSrcLive).table_rules);
  CHECK(PolicyFor(engine::kSrcLive).foreign);
  CHECK(std::string(PolicyFor(engine::kSrcLive).name) == "live");
}

TEST_CASE("A cell read from a file next to a table leaves the table as the file saved it") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "h1");
  t.Set("B1", "h2");
  t.Set("A2", "1");
  t.Set("B2", "2");
  REQUIRE(t.d.AddTable(sh, Rect{0, 0, 1, 1}, true, "TableStyleMedium2") != 0);
  t.d.SetValue(sh, 2, 0, yc::core::Value::Number(5), engine::kSrcFile);  // A3, under the table
  CHECK(t.d.S(sh)->tables->at(0).range == (Rect{0, 0, 1, 1}));
  t.d.SetValue(sh, 2, 1, yc::core::Value::Number(6), engine::kSrcPython);  // code writing there grows it, as Excel's
  CHECK(t.d.S(sh)->tables->at(0).range == (Rect{0, 0, 2, 1}));
}
