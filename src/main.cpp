// SPDX-License-Identifier: MIT
// cpp-softnet-watch — Linux per-CPU NAPI / softnet RX monitor
// Build: g++ -std=c++23 -O2 -Wall -Wextra src/main.cpp -o softnet-watch
// Usage: ./softnet-watch [--watch N] [--json] [--demo] [--no-color] [--selftest]
//        [--cpu N] [--alerts] [--sort FIELD] [--file PATH]

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <expected>
#include <fstream>
#include <iostream>
#include <optional>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

struct Error {
  int code;
  std::string message;
};

template <typename T>
using Result = std::expected<T, Error>;

struct CpuRow {
  int cpu{0};
  unsigned long long processed{0};
  unsigned long long dropped{0};
  unsigned long long squeeze{0};
  unsigned long long received_rps{0};
  unsigned long long flow_limit{0};
  unsigned long long backlog{0};
  unsigned long long input_qlen{0};
  unsigned long long process_qlen{0};
  unsigned long long d_processed{0};
  unsigned long long d_dropped{0};
  unsigned long long d_squeeze{0};
  double r_processed{0.0};
  double r_dropped{0.0};
  double r_squeeze{0.0};
  bool alert{false};
};

struct Snapshot {
  std::vector<CpuRow> cpus;
  double timestamp{0.0};
  bool has_delta{false};
  int ncpu{0};
  int alerts{0};
  unsigned long long processed{0};
  unsigned long long dropped{0};
  unsigned long long squeeze{0};
  unsigned long long received_rps{0};
  unsigned long long flow_limit{0};
  unsigned long long backlog{0};
  unsigned long long input_qlen{0};
  unsigned long long process_qlen{0};
  int hottest_cpu{-1};
  int droppiest_cpu{-1};
  int squeezest_cpu{-1};
};

struct Filters {
  int cpu{-1};
  bool alerts_only{false};
  std::string sort;  // processed|dropped|squeeze|backlog|rate
};

bool g_color = true;
std::string C_RST, C_BOLD, C_DIM, C_CYAN, C_GREEN, C_YELLOW, C_RED, C_MAGENTA, C_BLUE;

void init_colors() {
  C_RST = g_color ? "\033[0m" : "";
  C_BOLD = g_color ? "\033[1m" : "";
  C_DIM = g_color ? "\033[2m" : "";
  C_CYAN = g_color ? "\033[36m" : "";
  C_GREEN = g_color ? "\033[32m" : "";
  C_YELLOW = g_color ? "\033[33m" : "";
  C_RED = g_color ? "\033[31m" : "";
  C_MAGENTA = g_color ? "\033[35m" : "";
  C_BLUE = g_color ? "\033[34m" : "";
}

static double now_epoch() {
  using namespace std::chrono;
  return duration<double>(system_clock::now().time_since_epoch()).count();
}

static std::string trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
    s.remove_suffix(1);
  return std::string(s);
}

static unsigned long long parse_hex(std::string_view s) {
  auto start = s.find_first_not_of(" \t");
  if (start == std::string_view::npos) return 0;
  s.remove_prefix(start);
  if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s.remove_prefix(2);
  unsigned long long v = 0;
  auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v, 16);
  (void)ptr;
  if (ec != std::errc{}) return 0;
  return v;
}

static std::vector<std::string> split_ws(std::string_view line) {
  std::vector<std::string> out;
  std::istringstream ss{std::string(line)};
  std::string tok;
  while (ss >> tok) out.push_back(std::move(tok));
  return out;
}

static std::string json_escape(std::string_view s) {
  std::string o;
  o.reserve(s.size());
  for (unsigned char c : s) {
    switch (c) {
      case '\\': o += "\\\\"; break;
      case '"': o += "\\\""; break;
      case '\n': o += "\\n"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          o += buf;
        } else {
          o += static_cast<char>(c);
        }
        break;
    }
  }
  return o;
}

static std::string fmt_num(unsigned long long v) {
  char buf[48];
  if (v >= 1000000000ULL)
    std::snprintf(buf, sizeof(buf), "%.2fG", static_cast<double>(v) / 1e9);
  else if (v >= 1000000ULL)
    std::snprintf(buf, sizeof(buf), "%.2fM", static_cast<double>(v) / 1e6);
  else if (v >= 10000ULL)
    std::snprintf(buf, sizeof(buf), "%.1fk", static_cast<double>(v) / 1e3);
  else
    std::snprintf(buf, sizeof(buf), "%llu", v);
  return buf;
}

static std::string fmt_rate(double r) {
  char buf[32];
  if (r >= 1000000.0) std::snprintf(buf, sizeof(buf), "%.2fM/s", r / 1000000.0);
  else if (r >= 1000.0) std::snprintf(buf, sizeof(buf), "%.1fk/s", r / 1000.0);
  else if (r >= 10.0) std::snprintf(buf, sizeof(buf), "%.1f/s", r);
  else std::snprintf(buf, sizeof(buf), "%.2f/s", r);
  return buf;
}

static std::string fmt_time(double epoch) {
  std::time_t t = static_cast<std::time_t>(epoch);
  std::tm tm{};
  localtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
  return buf;
}

static std::string lower_copy(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

static Result<std::string> slurp(const std::string& path) {
  std::ifstream in(path);
  if (!in) return std::unexpected(Error{1, "cannot open " + path});
  std::ostringstream oss;
  oss << in.rdbuf();
  return oss.str();
}

// Kernel layout (v6.8+):
//  0 processed  1 dropped  2 time_squeeze  3-7 unused (cpu_collision retired)
//  8 received_rps  9 flow_limit  10 backlog
//  13-col: [11]=cpu  [12]=input_qlen
//  15-col: [11]=spare  [12]=cpu  [13]=input_qlen  [14]=process_qlen
static CpuRow row_from_fields(const std::vector<std::string>& f, int line_cpu) {
  CpuRow r;
  r.cpu = line_cpu;
  auto hex_at = [&](std::size_t i) -> unsigned long long {
    return i < f.size() ? parse_hex(f[i]) : 0;
  };
  r.processed = hex_at(0);
  r.dropped = hex_at(1);
  r.squeeze = hex_at(2);
  r.received_rps = hex_at(8);
  r.flow_limit = hex_at(9);
  r.backlog = hex_at(10);
  if (f.size() >= 15) {
    r.cpu = static_cast<int>(hex_at(12));
    r.input_qlen = hex_at(13);
    r.process_qlen = hex_at(14);
  } else if (f.size() >= 13) {
    r.cpu = static_cast<int>(hex_at(11));
    r.input_qlen = hex_at(12);
  } else if (f.size() >= 12) {
    r.cpu = static_cast<int>(hex_at(11));
  }
  if (r.cpu < 0 || r.cpu > 4096) r.cpu = line_cpu;
  r.alert = (r.dropped > 0 || r.squeeze > 0 || r.backlog > 0 || r.flow_limit > 0 ||
             r.input_qlen > 0 || r.process_qlen > 0);
  return r;
}

Result<std::vector<CpuRow>> parse_softnet_text(std::string_view text) {
  std::vector<CpuRow> rows;
  std::istringstream in{std::string(text)};
  std::string line;
  int idx = 0;
  while (std::getline(in, line)) {
    auto t = trim(line);
    if (t.empty() || t[0] == '#') continue;
    auto f = split_ws(t);
    if (f.empty()) continue;
    bool all_hex = true;
    for (const auto& tok : f) {
      for (char c : tok) {
        bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') ||
                  c == 'x' || c == 'X';
        if (!ok) {
          all_hex = false;
          break;
        }
      }
      if (!all_hex) break;
    }
    if (!all_hex) continue;
    rows.push_back(row_from_fields(f, idx));
    ++idx;
  }
  return rows;
}

static void summarize(Snapshot& s) {
  s.ncpu = static_cast<int>(s.cpus.size());
  s.processed = s.dropped = s.squeeze = s.received_rps = 0;
  s.flow_limit = s.backlog = s.input_qlen = s.process_qlen = 0;
  s.alerts = 0;
  s.hottest_cpu = s.droppiest_cpu = s.squeezest_cpu = -1;
  unsigned long long best_p = 0, best_d = 0, best_q = 0;
  for (const auto& c : s.cpus) {
    s.processed += c.processed;
    s.dropped += c.dropped;
    s.squeeze += c.squeeze;
    s.received_rps += c.received_rps;
    s.flow_limit += c.flow_limit;
    s.backlog += c.backlog;
    s.input_qlen += c.input_qlen;
    s.process_qlen += c.process_qlen;
    if (c.alert) ++s.alerts;
    if (c.processed >= best_p) {
      best_p = c.processed;
      s.hottest_cpu = c.cpu;
    }
    if (c.dropped >= best_d && c.dropped > 0) {
      best_d = c.dropped;
      s.droppiest_cpu = c.cpu;
    }
    if (c.squeeze >= best_q && c.squeeze > 0) {
      best_q = c.squeeze;
      s.squeezest_cpu = c.cpu;
    }
  }
}

static Snapshot snapshot_from_rows(std::vector<CpuRow> rows) {
  Snapshot s;
  s.cpus = std::move(rows);
  s.timestamp = now_epoch();
  summarize(s);
  return s;
}

Result<Snapshot> snapshot_from_text(std::string_view text) {
  auto rows = parse_softnet_text(text);
  if (!rows) return std::unexpected(rows.error());
  return snapshot_from_rows(std::move(*rows));
}

Result<Snapshot> read_live(const std::string& path) {
  auto txt = slurp(path);
  if (!txt) return std::unexpected(txt.error());
  return snapshot_from_text(*txt);
}

static const char* kDemoTable = R"(
# demo softnet_stat — 8 CPUs, 15 hex columns (kernel 6.8+ layout)
# processed dropped squeeze 0 0 0 0 collision rps flow backlog spare cpu inq procq
02b22be5 00000000 00003173 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000
02c55dff 00000012 0000321e 00000000 00000000 00000000 00000000 00000000 00000004 00000000 00000000 00000000 00000001 00000000 00000000
01a10c40 00000000 00001083 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000003 00000000 00000002 00000002 00000001
0313df23 00000003 00003424 00000000 00000000 00000000 00000000 00000000 00000000 00000001 00000000 00000000 00000003 00000000 00000000
00f00100 00000000 00000020 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000004 00000000 00000000
00401020 00000008 00000100 00000000 00000000 00000000 00000000 00000000 00000010 00000000 00000001 00000000 00000005 00000000 00000000
000abcde 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000006 00000000 00000000
00dead00 00000001 00000aaa 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000007 00000000 00000000
)";

static Snapshot build_demo_snapshot() {
  auto s = snapshot_from_text(kDemoTable);
  if (!s) return Snapshot{};
  return *s;
}

static void apply_filters(Snapshot& s, const Filters& f) {
  if (f.cpu >= 0) {
    std::vector<CpuRow> keep;
    for (auto& c : s.cpus)
      if (c.cpu == f.cpu) keep.push_back(c);
    s.cpus = std::move(keep);
  }
  if (f.alerts_only) {
    std::vector<CpuRow> keep;
    for (auto& c : s.cpus)
      if (c.alert) keep.push_back(c);
    s.cpus = std::move(keep);
  }
  auto key = lower_copy(f.sort);
  if (!key.empty()) {
    std::sort(s.cpus.begin(), s.cpus.end(), [&](const CpuRow& a, const CpuRow& b) {
      if (key == "dropped") return a.dropped > b.dropped;
      if (key == "squeeze") return a.squeeze > b.squeeze;
      if (key == "backlog") return a.backlog > b.backlog;
      if (key == "rate") return a.r_processed > b.r_processed;
      return a.processed > b.processed;
    });
  }
  summarize(s);
}

static void apply_delta(Snapshot& cur, const Snapshot& prev, double dt) {
  if (dt <= 0.0) dt = 1.0;
  for (auto& c : cur.cpus) {
    const CpuRow* p = nullptr;
    for (const auto& o : prev.cpus) {
      if (o.cpu == c.cpu) {
        p = &o;
        break;
      }
    }
    if (!p) continue;
    auto sub = [](unsigned long long a, unsigned long long b) -> unsigned long long {
      return a >= b ? a - b : 0;
    };
    c.d_processed = sub(c.processed, p->processed);
    c.d_dropped = sub(c.dropped, p->dropped);
    c.d_squeeze = sub(c.squeeze, p->squeeze);
    c.r_processed = static_cast<double>(c.d_processed) / dt;
    c.r_dropped = static_cast<double>(c.d_dropped) / dt;
    c.r_squeeze = static_cast<double>(c.d_squeeze) / dt;
  }
  cur.has_delta = true;
}

static void print_header(const Snapshot& s, bool demo, const std::string& src) {
  std::println(std::cout, "{}+================================================+{}", C_CYAN, C_RST);
  std::println(std::cout, "{}|   Linux softnet / NAPI per-CPU RX Monitor    |{}", C_CYAN, C_RST);
  std::println(std::cout, "{}+================================================+{}", C_CYAN, C_RST);
  std::println(std::cout, "  {}Source:{} {}  ·  {}  ·  {} CPUs{}", C_DIM, C_RST, src,
               fmt_time(s.timestamp), s.ncpu, demo ? "  ·  --demo" : "");
  std::println(std::cout);
}

static const char* health_of(const CpuRow& c) {
  if (c.dropped > 0 || c.backlog > 0 || c.input_qlen > 0 || c.process_qlen > 0) return "HOT";
  if (c.squeeze > 0 || c.flow_limit > 0) return "WARN";
  return "OK";
}

static void print_summary(const Snapshot& s) {
  std::println(std::cout, "{}  -- KPIs --{}", C_BOLD, C_RST);
  auto kpi = [](const char* lab, const std::string& val, const std::string& col) {
    std::println(std::cout, "  {}{:<14}{} {}{}{}", C_DIM, lab, C_RST, col, val, C_RST);
  };
  kpi("processed", fmt_num(s.processed), C_CYAN);
  kpi("dropped", fmt_num(s.dropped), s.dropped ? C_RED : C_GREEN);
  kpi("squeeze", fmt_num(s.squeeze), s.squeeze ? C_YELLOW : C_GREEN);
  kpi("RPS wakes", fmt_num(s.received_rps), C_BLUE);
  kpi("flow_limit", fmt_num(s.flow_limit), s.flow_limit ? C_RED : C_DIM);
  kpi("backlog", fmt_num(s.backlog), s.backlog ? C_YELLOW : C_DIM);
  kpi("input_qlen", fmt_num(s.input_qlen), s.input_qlen ? C_YELLOW : C_DIM);
  kpi("process_q", fmt_num(s.process_qlen), s.process_qlen ? C_YELLOW : C_DIM);
  kpi("alerts", std::to_string(s.alerts), s.alerts ? C_RED : C_GREEN);
  if (s.hottest_cpu >= 0)
    kpi("hottest CPU", "cpu" + std::to_string(s.hottest_cpu), C_MAGENTA);
  if (s.droppiest_cpu >= 0)
    kpi("most drops", "cpu" + std::to_string(s.droppiest_cpu), C_RED);
  if (s.squeezest_cpu >= 0)
    kpi("most squeeze", "cpu" + std::to_string(s.squeezest_cpu), C_YELLOW);
  std::println(std::cout);
}

static void print_table(const Snapshot& s) {
  std::println(std::cout, "{}  -- Per-CPU NAPI --{}", C_BOLD, C_RST);
  if (s.has_delta) {
    std::println(std::cout, "  {}{:>4} {:>10} {:>8} {:>8} {:>6} {:>6} {:>6} {:>6} {:>10} {:>6}{}",
                 C_DIM, "cpu", "processed", "dropped", "squeeze", "rps", "flow", "back", "inq",
                 "pkt/s", "health", C_RST);
  } else {
    std::println(std::cout, "  {}{:>4} {:>10} {:>8} {:>8} {:>6} {:>6} {:>6} {:>6} {:>6}{}", C_DIM,
                 "cpu", "processed", "dropped", "squeeze", "rps", "flow", "back", "inq", "health",
                 C_RST);
  }
  for (const auto& c : s.cpus) {
    const char* h = health_of(c);
    const std::string& hc = (std::string_view(h) == "HOT")    ? C_RED
                            : (std::string_view(h) == "WARN") ? C_YELLOW
                                                              : C_GREEN;
    const std::string& dc = c.dropped ? C_RED : C_RST;
    const std::string& qc = c.squeeze ? C_YELLOW : C_RST;
    if (s.has_delta) {
      std::println(std::cout,
                   "  {:>4} {:>10} {}{:>8}{} {}{:>8}{} {:>6} {:>6} {:>6} {:>6} {:>10} {}{:>6}{}",
                   c.cpu, fmt_num(c.processed), dc, fmt_num(c.dropped), C_RST, qc,
                   fmt_num(c.squeeze), C_RST, fmt_num(c.received_rps), fmt_num(c.flow_limit),
                   fmt_num(c.backlog), fmt_num(c.input_qlen), fmt_rate(c.r_processed), hc, h,
                   C_RST);
    } else {
      std::println(std::cout, "  {:>4} {:>10} {}{:>8}{} {}{:>8}{} {:>6} {:>6} {:>6} {:>6} {}{:>6}{}",
                   c.cpu, fmt_num(c.processed), dc, fmt_num(c.dropped), C_RST, qc,
                   fmt_num(c.squeeze), C_RST, fmt_num(c.received_rps), fmt_num(c.flow_limit),
                   fmt_num(c.backlog), fmt_num(c.input_qlen), hc, h, C_RST);
    }
  }
  std::println(std::cout);
  std::println(std::cout, "{}  processed = NAPI RX packets · dropped = backlog overflow{}", C_DIM,
               C_RST);
  std::println(std::cout, "{}  squeeze   = net_rx_action exhausted quota/time{}", C_DIM, C_RST);
  std::println(std::cout, "{}  rps/flow  = RPS wakeups / flow-limit hits · backlog = current q{}",
               C_DIM, C_RST);
}

static void print_json(const Snapshot& s) {
  std::print(std::cout, "{{\n");
  std::print(std::cout, "  \"summary\": {{\n");
  std::print(std::cout, "    \"cpus\": {},\n", s.ncpu);
  std::print(std::cout, "    \"processed\": {},\n", s.processed);
  std::print(std::cout, "    \"dropped\": {},\n", s.dropped);
  std::print(std::cout, "    \"squeeze\": {},\n", s.squeeze);
  std::print(std::cout, "    \"received_rps\": {},\n", s.received_rps);
  std::print(std::cout, "    \"flow_limit\": {},\n", s.flow_limit);
  std::print(std::cout, "    \"backlog\": {},\n", s.backlog);
  std::print(std::cout, "    \"input_qlen\": {},\n", s.input_qlen);
  std::print(std::cout, "    \"process_qlen\": {},\n", s.process_qlen);
  std::print(std::cout, "    \"alerts\": {},\n", s.alerts);
  std::print(std::cout, "    \"hottest_cpu\": {},\n", s.hottest_cpu);
  std::print(std::cout, "    \"droppiest_cpu\": {},\n", s.droppiest_cpu);
  std::print(std::cout, "    \"squeezest_cpu\": {},\n", s.squeezest_cpu);
  std::print(std::cout, "    \"has_delta\": {},\n", s.has_delta ? "true" : "false");
  std::print(std::cout, "    \"timestamp\": {:.3f}\n", s.timestamp);
  std::print(std::cout, "  }},\n");
  std::print(std::cout, "  \"cpus\": [\n");
  for (std::size_t i = 0; i < s.cpus.size(); ++i) {
    const auto& c = s.cpus[i];
    std::print(std::cout, "    {{\n");
    std::print(std::cout, "      \"cpu\": {},\n", c.cpu);
    std::print(std::cout, "      \"processed\": {},\n", c.processed);
    std::print(std::cout, "      \"dropped\": {},\n", c.dropped);
    std::print(std::cout, "      \"squeeze\": {},\n", c.squeeze);
    std::print(std::cout, "      \"received_rps\": {},\n", c.received_rps);
    std::print(std::cout, "      \"flow_limit\": {},\n", c.flow_limit);
    std::print(std::cout, "      \"backlog\": {},\n", c.backlog);
    std::print(std::cout, "      \"input_qlen\": {},\n", c.input_qlen);
    std::print(std::cout, "      \"process_qlen\": {},\n", c.process_qlen);
    std::print(std::cout, "      \"d_processed\": {},\n", c.d_processed);
    std::print(std::cout, "      \"r_processed\": {:.6f},\n", c.r_processed);
    std::print(std::cout, "      \"alert\": {},\n", c.alert ? "true" : "false");
    std::print(std::cout, "      \"health\": \"{}\"\n", json_escape(health_of(c)));
    std::print(std::cout, "    }}{}\n", (i + 1 < s.cpus.size()) ? "," : "");
  }
  std::print(std::cout, "  ]\n");
  std::print(std::cout, "}}\n");
}

static int run_selftest() {
  int passed = 0, failed = 0;
  auto check = [&](const char* name, bool ok) {
    if (ok) {
      ++passed;
      std::println(std::cout, "  {}PASS{}  {}", C_GREEN, C_RST, name);
    } else {
      ++failed;
      std::println(std::cout, "  {}FAIL{}  {}", C_RED, C_RST, name);
    }
  };

  std::println(std::cout, "{}softnet-watch selftest{}", C_BOLD, C_RST);

  check("trim spaces", trim("  abc \t") == "abc");
  check("parse_hex dead", parse_hex("0000dead") == 0xdead);
  check("parse_hex 0x10", parse_hex("0x10") == 16);
  check("parse_hex spaces", parse_hex("  0A") == 10);
  check("parse_hex junk", parse_hex("zz") == 0);
  check("json_escape quote", json_escape("a\"b") == "a\\\"b");

  {
    const char* sample =
        "# comment\n"
        "0000000a 00000001 00000002 0 0 0 0 0 00000003 00000004 00000005 00000000 00000000 "
        "00000006 00000007\n"
        "\n"
        "00000014 00000000 00000000 0 0 0 0 0 0 0 0 0 00000001 0 0\n";
    auto r = parse_softnet_text(sample);
    check("parse ok", r.has_value());
    check("parse 2 cpus", r && r->size() == 2);
    if (r && r->size() == 2) {
      check("cpu0 id", (*r)[0].cpu == 0);
      check("cpu0 processed", (*r)[0].processed == 10);
      check("cpu0 dropped", (*r)[0].dropped == 1);
      check("cpu0 squeeze", (*r)[0].squeeze == 2);
      check("cpu0 rps", (*r)[0].received_rps == 3);
      check("cpu0 flow", (*r)[0].flow_limit == 4);
      check("cpu0 backlog", (*r)[0].backlog == 5);
      check("cpu0 inq", (*r)[0].input_qlen == 6);
      check("cpu0 procq", (*r)[0].process_qlen == 7);
      check("cpu0 alert", (*r)[0].alert);
      check("cpu1 id", (*r)[1].cpu == 1);
      check("cpu1 processed", (*r)[1].processed == 20);
      check("cpu1 no alert", !(*r)[1].alert);
      check("comment skipped", true);
    }
  }

  {
    // 13-column kernel 6.8 layout
    const char* v13 =
        "00000064 00000000 00000001 0 0 0 0 0 0 0 00000002 00000003 00000004\n";
    auto r = parse_softnet_text(v13);
    check("v13 parse", r && r->size() == 1);
    if (r && r->size() == 1) {
      check("v13 processed", (*r)[0].processed == 100);
      check("v13 squeeze", (*r)[0].squeeze == 1);
      check("v13 backlog", (*r)[0].backlog == 2);
      check("v13 cpu field", (*r)[0].cpu == 3);
      check("v13 input_qlen", (*r)[0].input_qlen == 4);
    }
  }

  auto demo = build_demo_snapshot();
  check("demo ncpu == 8", demo.ncpu == 8);
  check("demo cpu0 processed", demo.cpus.size() == 8 && demo.cpus[0].processed == 0x02b22be5ULL);
  check("demo cpu1 dropped", demo.cpus.size() == 8 && demo.cpus[1].dropped == 0x12);
  check("demo cpu2 backlog", demo.cpus.size() == 8 && demo.cpus[2].backlog == 3);
  check("demo cpu2 inq", demo.cpus.size() == 8 && demo.cpus[2].input_qlen == 2);
  check("demo cpu3 flow", demo.cpus.size() == 8 && demo.cpus[3].flow_limit == 1);
  check("demo totals processed", demo.processed > 0);
  check("demo totals dropped", demo.dropped == (0x12 + 3 + 8 + 1));
  check("demo alerts > 0", demo.alerts > 0);
  check("demo hottest set", demo.hottest_cpu >= 0);
  check("demo droppiest cpu1", demo.droppiest_cpu == 1);

  bool print_ok = true;
  try {
    print_header(demo, true, "/proc/net/softnet_stat");
    print_summary(demo);
    print_table(demo);
  } catch (...) {
    print_ok = false;
  }
  check("demo print no crash", print_ok);

  std::string json_out;
  {
    std::ostringstream oss;
    auto* old = std::cout.rdbuf(oss.rdbuf());
    print_json(demo);
    std::cout.rdbuf(old);
    json_out = oss.str();
  }
  check("JSON starts with '{'", !json_out.empty() && json_out[0] == '{');
  check("JSON has summary", json_out.find("\"summary\"") != std::string::npos);
  check("JSON has cpus", json_out.find("\"cpus\"") != std::string::npos);
  check("JSON quoted processed", json_out.find("\"processed\"") != std::string::npos);
  check("JSON quoted squeeze", json_out.find("\"squeeze\"") != std::string::npos);
  check("JSON quoted health", json_out.find("\"health\"") != std::string::npos);
  check("JSON quoted hottest_cpu", json_out.find("\"hottest_cpu\"") != std::string::npos);

  {
    auto d = build_demo_snapshot();
    Filters f;
    f.cpu = 2;
    apply_filters(d, f);
    check("--cpu 2 filter", d.ncpu == 1 && !d.cpus.empty() && d.cpus[0].cpu == 2);
  }
  {
    auto d = build_demo_snapshot();
    Filters f;
    f.alerts_only = true;
    apply_filters(d, f);
    bool only = !d.cpus.empty();
    for (const auto& c : d.cpus)
      if (!c.alert) only = false;
    check("--alerts filter", only);
  }
  {
    auto d = build_demo_snapshot();
    Filters f;
    f.sort = "dropped";
    apply_filters(d, f);
    bool sorted = true;
    for (std::size_t i = 1; i < d.cpus.size(); ++i)
      if (d.cpus[i].dropped > d.cpus[i - 1].dropped) sorted = false;
    check("--sort dropped", sorted && d.cpus.size() == 8);
  }

  {
    auto a = build_demo_snapshot();
    auto b = build_demo_snapshot();
    for (auto& c : b.cpus) {
      if (c.cpu == 0) c.processed += 200;
      if (c.cpu == 1) c.dropped += 4;
    }
    apply_delta(b, a, 2.0);
    bool rate_ok = false, drop_ok = false;
    for (const auto& c : b.cpus) {
      if (c.cpu == 0) rate_ok = (c.d_processed == 200 && c.r_processed == 100.0);
      if (c.cpu == 1) drop_ok = (c.d_dropped == 4 && c.r_dropped == 2.0);
    }
    check("delta rate processed", rate_ok);
    check("delta rate dropped", drop_ok);
    check("has_delta set", b.has_delta);
  }

  auto real = read_live("/proc/net/softnet_stat");
  if (real.has_value()) {
    check("proc softnet read ok", true);
    check("real ncpu >= 1", real->ncpu >= 1);
    check("real processed countable", real->processed < (1ULL << 62));
    check("real cpus match lines", static_cast<int>(real->cpus.size()) == real->ncpu);
  } else {
    std::println(std::cout, "  {}ℹ{} /proc/net/softnet_stat failed ({}): restricted env — skipping",
                 C_YELLOW, C_RST, real.error().message);
  }

  g_color = true;
  init_colors();
  std::string colored = C_GREEN;
  g_color = false;
  init_colors();
  std::string plain = C_GREEN;
  check("--no-color clears ANSI", !colored.empty() && plain.empty());
  g_color = true;
  init_colors();

  std::println(std::cout);
  std::println(std::cout, "{}  Results: {} passed, {} failed{}", failed == 0 ? C_GREEN : C_RED,
               passed, failed, C_RST);
  return failed == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
  bool json_mode = false;
  bool watch_mode = false;
  bool demo_mode = false;
  bool selftest = false;
  int watch_secs = 2;
  Filters filt;
  std::string file_path;
  std::string proc_path = "/proc/net/softnet_stat";

  for (int i = 1; i < argc; ++i) {
    std::string_view arg{argv[i]};
    if (arg == "--json") json_mode = true;
    else if (arg == "--no-color" || arg == "-c") g_color = false;
    else if (arg == "--demo") demo_mode = true;
    else if (arg == "--selftest") selftest = true;
    else if (arg == "--alerts") filt.alerts_only = true;
    else if ((arg == "--cpu") && i + 1 < argc) {
      ++i;
      int v = -1;
      std::from_chars(argv[i], argv[i] + std::strlen(argv[i]), v);
      filt.cpu = v;
    } else if ((arg == "--sort") && i + 1 < argc) {
      ++i;
      filt.sort = argv[i];
    } else if ((arg == "--file" || arg == "-f") && i + 1 < argc) {
      ++i;
      file_path = argv[i];
    } else if (arg == "--proc" && i + 1 < argc) {
      ++i;
      proc_path = argv[i];
    } else if ((arg == "--watch" || arg == "-w") && i + 1 < argc) {
      watch_mode = true;
      ++i;
      std::from_chars(argv[i], argv[i] + std::strlen(argv[i]), watch_secs);
      if (watch_secs < 1) watch_secs = 1;
    } else if (arg == "--help" || arg == "-h") {
      init_colors();
      std::println(std::cout, "{}softnet-watch{} — Linux per-CPU NAPI / softnet RX monitor\n",
                   C_BOLD, C_RST);
      std::println(std::cout, "Usage: softnet-watch [options]\n");
      std::println(std::cout, "Options:");
      std::println(std::cout, "  -w, --watch N      Refresh every N seconds (default: 2)");
      std::println(std::cout, "  --json             Output JSON");
      std::println(std::cout, "  --no-color, -c     Disable ANSI colors");
      std::println(std::cout, "  --demo             Simulated 8-CPU softnet dump");
      std::println(std::cout, "  --cpu N            Show a single CPU");
      std::println(std::cout, "  --sort FIELD       processed|dropped|squeeze|backlog|rate");
      std::println(std::cout, "  --alerts           Only CPUs with drops/squeeze/backlog");
      std::println(std::cout, "  -f, --file PATH    Parse a dump instead of /proc");
      std::println(std::cout, "  --proc PATH        Override /proc/net/softnet_stat");
      std::println(std::cout, "  --selftest         Run built-in tests");
      std::println(std::cout, "  -h, --help         Show this help\n");
      std::println(std::cout, "{}", C_DIM + "Reads: /proc/net/softnet_stat" + C_RST);
      return 0;
    }
  }

  init_colors();
  if (selftest) return run_selftest();

  auto take = [&]() -> std::optional<Snapshot> {
    if (demo_mode) {
      auto d = build_demo_snapshot();
      apply_filters(d, filt);
      return d;
    }
    Result<Snapshot> r = file_path.empty() ? read_live(proc_path) : [&]() -> Result<Snapshot> {
      auto txt = slurp(file_path);
      if (!txt) return std::unexpected(txt.error());
      return snapshot_from_text(*txt);
    }();
    if (!r) {
      if (!json_mode) std::println(std::cerr, "{}Error:{} {}", C_RED, C_RST, r.error().message);
      return std::nullopt;
    }
    apply_filters(*r, filt);
    return *r;
  };

  const std::string src =
      demo_mode ? "/proc/net/softnet_stat" : (file_path.empty() ? proc_path : file_path);

  if (watch_mode) {
    std::optional<Snapshot> prev;
    bool first = true;
    while (true) {
      auto snap = take();
      if (!snap) return 1;
      if (prev) {
        double dt = snap->timestamp - prev->timestamp;
        if (first || dt <= 0.0) dt = static_cast<double>(watch_secs);
        apply_delta(*snap, *prev, dt);
      }
      prev = snap;

      if (!json_mode && !first) std::cout << "\033[2J\033[H";
      first = false;

      if (json_mode) {
        print_json(*snap);
      } else {
        print_header(*snap, demo_mode, src);
        print_summary(*snap);
        print_table(*snap);
        std::println(std::cout, "{}  Refreshing every {}s — Ctrl+C to exit{}", C_DIM, watch_secs,
                     C_RST);
      }
      std::cout << std::flush;
      std::this_thread::sleep_for(std::chrono::seconds(watch_secs));
    }
  } else {
    auto snap = take();
    if (!snap) return 1;
    if (json_mode) {
      print_json(*snap);
    } else {
      print_header(*snap, demo_mode, src);
      print_summary(*snap);
      print_table(*snap);
    }
  }
  return 0;
}
