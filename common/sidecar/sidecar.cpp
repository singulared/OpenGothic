#include "sidecar.h"

#include <Tempest/Log>

// The vendored rapidjson 1.1.0 predates the diagnostics of current GCC
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#pragma GCC diagnostic ignored "-Wtemplate-body"
#endif
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <unordered_set>

#if defined(__linux__)
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include "game/gamescript.h"
#include "world/objects/interactive.h"
#include "world/objects/npc.h"
#include "world/aiqueue.h"
#include "world/waypoint.h"
#include "world/world.h"
#include "commandline.h"

using namespace Tempest;

namespace {

constexpr uint32_t    ProtocolVersion   = 0;
constexpr size_t      MaxFrame          = size_t(1) << 20;
constexpr auto        ReconnectInterval = std::chrono::seconds(2);
// World ticks, in milliseconds: they stop while the game is paused, wall time doesn't
constexpr uint64_t    DecideTimeout     = 3'000;
constexpr uint64_t    RunTimeout        = 60'000;
constexpr uint64_t    TelemetryInterval = 1'000;
constexpr size_t      MaxQueueShown     = 8;
constexpr size_t      MaxWaypoints      = 8;
constexpr int         WaypointHops      = 2;
constexpr float       ArrivedDistance   = 200.f; // centimetres
constexpr int         SayTime           = 3;     // seconds
constexpr int         SayPosY           = 75;    // percent of the screen height
constexpr const char* SayFont           = "font_old_10_white.tga";

// Values of an aborted result's `reason`
constexpr const char* AbortQueueCleared  = "queue cleared";
constexpr const char* AbortQueueRanEmpty = "queue ran empty";
constexpr const char* AbortTimeout       = "timeout";

// Unicode code points of CP1251 bytes 0x80..0xFF; 0 marks an unassigned byte.
// v0 supports only the Russian code page.
constexpr uint16_t cp1251[128] = {
  0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021,
  0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F,
  0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
  0x0000, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F,
  0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7,
  0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407,
  0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7,
  0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457,
  0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417,
  0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E, 0x041F,
  0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427,
  0x0428, 0x0429, 0x042A, 0x042B, 0x042C, 0x042D, 0x042E, 0x042F,
  0x0430, 0x0431, 0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437,
  0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E, 0x043F,
  0x0440, 0x0441, 0x0442, 0x0443, 0x0444, 0x0445, 0x0446, 0x0447,
  0x0448, 0x0449, 0x044A, 0x044B, 0x044C, 0x044D, 0x044E, 0x044F,
  };

using Writer = rapidjson::Writer<rapidjson::StringBuffer>;

void appendUtf8(std::string& out, uint32_t cp) {
  // CP1251 maps only into the BMP, so three bytes are enough
  if(cp<0x80) {
    out.push_back(char(cp));
    }
  else if(cp<0x800) {
    out.push_back(char(0xC0 | (cp>>6)));
    out.push_back(char(0x80 | (cp & 0x3F)));
    }
  else {
    out.push_back(char(0xE0 | (cp>>12)));
    out.push_back(char(0x80 | ((cp>>6) & 0x3F)));
    out.push_back(char(0x80 | (cp & 0x3F)));
    }
  }

// Game text (CP1251) to UTF-8.
std::string toUtf8(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for(char ch:s) {
    auto b = uint8_t(ch);
    if(b<0x80) {
      out.push_back(ch);
      continue;
      }
    uint32_t cp = cp1251[b-0x80];
    appendUtf8(out, cp!=0 ? cp : uint32_t('?'));
    }
  return out;
  }

char fromCodePoint(uint32_t cp) {
  if(cp<0x80)
    return char(cp);
  for(size_t i=0; i<128; ++i)
    if(cp1251[i]==cp)
      return char(0x80+i);
  return '?';
  }

// UTF-8 to game text (CP1251); characters CP1251 lacks become '?'.
std::string fromUtf8(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for(size_t i=0; i<s.size();) {
    auto     b   = uint8_t(s[i]);
    uint32_t cp  = 0;
    size_t   len = 1;
    if(b<0x80) {
      cp = b;
      }
    else if((b>>5)==0x6) {
      cp  = b & 0x1Fu;
      len = 2;
      }
    else if((b>>4)==0xE) {
      cp  = b & 0x0Fu;
      len = 3;
      }
    else if((b>>3)==0x1E) {
      cp  = b & 0x07u;
      len = 4;
      }
    else {
      // stray continuation byte
      out.push_back('?');
      ++i;
      continue;
      }
    if(i+len>s.size()) {
      out.push_back('?');
      break;
      }
    for(size_t k=1; k<len; ++k)
      cp = (cp<<6) | (uint8_t(s[i+k]) & 0x3Fu);
    i += len;
    out.push_back(fromCodePoint(cp));
    }
  return out;
  }

// Writes game text as a JSON string.
void writeText(Writer& w, std::string_view text) {
  auto s = toUtf8(text);
  w.String(s.data(), rapidjson::SizeType(s.size()));
  }

std::string_view str(const rapidjson::Value& v, const char* key) {
  auto it = v.FindMember(key);
  if(it==v.MemberEnd() || !it->value.IsString())
    return {};
  return {it->value.GetString(), it->value.GetStringLength()};
  }

bool u32(const rapidjson::Value& v, const char* key, uint32_t& out) {
  auto it = v.FindMember(key);
  if(it==v.MemberEnd() || !it->value.IsUint())
    return false;
  out = it->value.GetUint();
  return true;
  }

std::string_view instanceName(Npc& npc) {
  auto* sym = npc.world().script().findSymbol(size_t(npc.instanceSymbol()));
  return sym!=nullptr ? std::string_view(sym->name()) : std::string_view();
  }

Npc* findNpc(World& world, std::string_view name) {
  auto id = world.script().findSymbolIndex(name);
  return id==size_t(-1) ? nullptr : world.findNpcByInstance(id);
  }

const char* perceptionName(PercType perc) {
  switch(perc) {
    case PERC_ASSESSPLAYER:       return "assess_player";
    case PERC_ASSESSENEMY:        return "assess_enemy";
    case PERC_ASSESSFIGHTER:      return "assess_fighter";
    case PERC_ASSESSBODY:         return "assess_body";
    case PERC_ASSESSITEM:         return "assess_item";
    case PERC_ASSESSMURDER:       return "assess_murder";
    case PERC_ASSESSDEFEAT:       return "assess_defeat";
    case PERC_ASSESSDAMAGE:       return "assess_damage";
    case PERC_ASSESSOTHERSDAMAGE: return "assess_others_damage";
    case PERC_ASSESSTHREAT:       return "assess_threat";
    case PERC_ASSESSREMOVEWEAPON: return "assess_remove_weapon";
    case PERC_OBSERVEINTRUDER:    return "observe_intruder";
    case PERC_ASSESSFIGHTSOUND:   return "assess_fight_sound";
    case PERC_ASSESSQUIETSOUND:   return "assess_quiet_sound";
    case PERC_ASSESSWARN:         return "assess_warn";
    case PERC_CATCHTHIEF:         return "catch_thief";
    case PERC_ASSESSTHEFT:        return "assess_theft";
    case PERC_ASSESSCALL:         return "assess_call";
    case PERC_ASSESSTALK:         return "assess_talk";
    case PERC_ASSESSGIVENITEM:    return "assess_given_item";
    case PERC_ASSESSFAKEGUILD:    return "assess_fake_guild";
    case PERC_MOVEMOB:            return "move_mob";
    case PERC_MOVENPC:            return "move_npc";
    case PERC_DRAWWEAPON:         return "draw_weapon";
    case PERC_OBSERVESUSPECT:     return "observe_suspect";
    case PERC_NPCCOMMAND:         return "npc_command";
    case PERC_ASSESSMAGIC:        return "assess_magic";
    case PERC_ASSESSSTOPMAGIC:    return "assess_stop_magic";
    case PERC_ASSESSCASTER:       return "assess_caster";
    case PERC_ASSESSSURPRISE:     return "assess_surprise";
    case PERC_ASSESSENTERROOM:    return "assess_enter_room";
    case PERC_ASSESSUSEMOB:       return "assess_use_mob";
    case PERC_None:
    case PERC_Count:
      break;
    }
  return "unknown";
  }

const char* aiName(NpcProcessPolicy p) {
  switch(p) {
    case NpcProcessPolicy::Player:   return "player";
    case NpcProcessPolicy::AiNormal: return "normal";
    case NpcProcessPolicy::AiFar:    return "far";
    case NpcProcessPolicy::AiFar2:   return "far2";
    }
  return "unknown";
  }

const char* bodyStateName(BodyState bs) {
  static const char* names[] = {
    "stand", "walk", "sneak", "run", "sprint", "swim", "crawl", "dive", "jump", "climb",
    "fall", "sit", "lie", "inventory", "item_interact", "mob_interact",
    "mob_interact_interrupt", "take_item", "drop_item", "throw_item", "pickpocket",
    "stumble", "unconscious", "dead", "aim_near", "aim_far", "hit", "parade", "casting",
    "petrified", "controlling",
    };
  auto i = size_t(bs & BS_MAX);
  return i<std::size(names) ? names[i] : "unknown";
  }

const char* actionName(Action a) {
  switch(a) {
    case AI_None:             return "none";
    case AI_LookAtNpc:        return "look_at_npc";
    case AI_StopLookAt:       return "stop_look_at";
    case AI_RemoveWeapon:     return "remove_weapon";
    case AI_TurnToNpc:        return "turn_to_npc";
    case AI_GoToNpc:          return "go_to_npc";
    case AI_GoToNextFp:       return "go_to_next_fp";
    case AI_GoToPoint:        return "go_to_point";
    case AI_StartState:       return "start_state";
    case AI_PlayAnim:         return "play_anim";
    case AI_PlayAnimBs:       return "play_anim_bs";
    case AI_Wait:             return "wait";
    case AI_StandUp:          return "stand_up";
    case AI_StandUpQuick:     return "stand_up_quick";
    case AI_EquipArmor:       return "equip_armor";
    case AI_EquipBestArmor:   return "equip_best_armor";
    case AI_EquipMelee:       return "equip_melee";
    case AI_EquipRange:       return "equip_range";
    case AI_UseMob:           return "use_mob";
    case AI_UseItem:          return "use_item";
    case AI_UseItemToState:   return "use_item_to_state";
    case AI_Teleport:         return "teleport";
    case AI_DrawWeaponMelee:  return "draw_weapon_melee";
    case AI_DrawWeaponRange:  return "draw_weapon_range";
    case AI_DrawSpell:        return "draw_spell";
    case AI_Attack:           return "attack";
    case AI_Flee:             return "flee";
    case AI_Dodge:            return "dodge";
    case AI_UnEquipWeapons:   return "unequip_weapons";
    case AI_UnEquipArmor:     return "unequip_armor";
    case AI_Output:           return "output";
    case AI_OutputSvm:        return "output_svm";
    case AI_OutputSvmOverlay: return "output_svm_overlay";
    case AI_ProcessInfo:      return "process_info";
    case AI_StopProcessInfo:  return "stop_process_info";
    case AI_ContinueRoutine:  return "continue_routine";
    case AI_AlignToFp:        return "align_to_fp";
    case AI_AlignToWp:        return "align_to_wp";
    case AI_SetNpcsToState:   return "set_npcs_to_state";
    case AI_SetWalkMode:      return "set_walk_mode";
    case AI_FinishingMove:    return "finishing_move";
    case AI_DrawWeapon:       return "draw_weapon";
    case AI_TakeItem:         return "take_item";
    case AI_GotoItem:         return "go_to_item";
    case AI_PointAtNpc:       return "point_at_npc";
    case AI_PointAt:          return "point_at";
    case AI_StopPointAt:      return "stop_point_at";
    case AI_PrintScreen:      return "print_screen";
    case AI_LookAt:           return "look_at";
    case AI_WhirlToNpc:       return "whirl_to_npc";
    case AI_TurnAway:         return "turn_away";
    case AI_SidecarDone:      return "sidecar_done";
    }
  return "unknown";
  }

// Name of a script function such as an AI state; empty when unknown.
std::string_view symbolName(World& world, const ScriptFn& fn) {
  if(!fn.isValid())
    return {};
  auto* sym = world.script().findSymbol(fn.ptr);
  return sym!=nullptr ? std::string_view(sym->name()) : std::string_view();
  }

void writePosition(Writer& w, const Npc& npc) {
  auto pos = npc.position();
  w.Key("position");
  w.StartArray();
  w.Double(double(pos.x));
  w.Double(double(pos.y));
  w.Double(double(pos.z));
  w.EndArray();
  }

void writeName(Writer& w, const char* key, std::string_view name) {
  if(name.empty())
    return;
  w.Key(key);
  writeText(w,name);
  }

bool isReachable(World& world, const Npc& npc, const WayPoint& wp) {
  if(npc.qDistTo(&wp)<ArrivedDistance*ArrivedDistance)
    return true;
  return world.wayTo(npc,wp).first()!=nullptr;
  }

struct Nearby {
  const WayPoint* point    = nullptr;
  float           distance = 0;
  };

// Graph neighbours of `start` within a few hops, nearest first: a canonical candidate order.
std::vector<Nearby> neighbours(const Npc& npc, const WayPoint* start) {
  std::vector<Nearby> out;
  if(start==nullptr)
    return out;

  std::unordered_set<const WayPoint*> seen     = {start};
  std::vector<const WayPoint*>        frontier = {start};
  for(int hop=0; hop<WaypointHops; ++hop) {
    std::vector<const WayPoint*> next;
    for(auto* p:frontier) {
      for(auto& conn:p->connections()) {
        if(conn.point==nullptr || !seen.insert(conn.point).second)
          continue;
        next.push_back(conn.point);
        out.push_back({conn.point, std::sqrt(npc.qDistTo(conn.point))});
        }
      }
    frontier = std::move(next);
    }

  std::sort(out.begin(), out.end(), [](const Nearby& a, const Nearby& b) {
    if(a.distance!=b.distance)
      return a.distance<b.distance;
    return a.point->name<b.point->name;
    });
  if(out.size()>MaxWaypoints)
    out.resize(MaxWaypoints);
  return out;
  }

}

Sidecar& Sidecar::inst() {
  static Sidecar s;
  return s;
  }

Sidecar::Sidecar()
  : socketPath(CommandLine::inst().sidecarSocket()) {
  }

Sidecar::~Sidecar() {
#if defined(__linux__)
  if(fd>=0)
    ::close(fd);
#endif
  }

void Sidecar::tick(World& world) {
  if(socketPath.empty())
    return;

  if(&world!=lastWorld) {
    // new game, loaded save or another world: the Npc objects are new
    lastWorld     = &world;
    nextTelemetry = 0;
    for(auto& [id,c]:controlled) {
      setMode(world,c,Mode::Idle);
      c.hasPending = false;
      }
    }

  if(fd<0)
    connect(world);
  if(fd>=0)
    receive(world);
  if(fd>=0 && isActive()) {
    checkDeadlines(world);
    if(telemetryDirty || world.tickCount()>=nextTelemetry) {
      sendTelemetry(world);
      nextTelemetry  = world.tickCount() + TelemetryInterval;
      telemetryDirty = false;
      }
    }
  if(fd>=0)
    flush();
  }

void Sidecar::connect(World& world) {
#if defined(__linux__)
  auto now = Clock::now();
  if(now<nextConnect)
    return;
  nextConnect = now + ReconnectInterval;

  sockaddr_un addr = {};
  addr.sun_family = AF_UNIX;
  if(socketPath.size()>=sizeof(addr.sun_path)) {
    Log::e("sidecar: socket path is too long: ", socketPath);
    socketPath.clear();
    return;
    }
  std::memcpy(addr.sun_path, socketPath.data(), socketPath.size());

  int s = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if(s<0)
    return;
  if(::connect(s, reinterpret_cast<const sockaddr*>(&addr), socklen_t(sizeof(addr)))!=0) {
    ::close(s);
    return;
    }
  fd = s;

  rapidjson::StringBuffer buf;
  Writer w(buf);
  w.StartObject();
  w.Key("type");     w.String("hello");
  w.Key("protocol"); w.Uint(ProtocolVersion);
  w.Key("engine");   w.String("opengothic");
  w.Key("world");    writeText(w, world.name());
  w.EndObject();
  send({buf.GetString(), buf.GetSize()});
  // after hello: the sidecar expects it first
  note(false, "sidecar: connected to "+socketPath);
#else
  (void)world;
  Log::e("sidecar: only supported on Linux");
  socketPath.clear();
#endif
  }

void Sidecar::disconnect(std::string_view reason) {
#if defined(__linux__)
  if(fd>=0)
    ::close(fd);
#endif
  fd = -1;
  inBuf.clear();
  outBuf.clear();
  // every NPC returns to vanilla; completion markers already queued are ignored
  controlled.clear();
  Log::i("sidecar: disconnected: ", reason);
  }

void Sidecar::receive(World& world) {
#if defined(__linux__)
  char buf[16*1024];
  while(true) {
    auto n = ::recv(fd, buf, sizeof(buf), 0);
    if(n>0) {
      inBuf.insert(inBuf.end(), buf, buf+n);
      continue;
      }
    if(n==0)
      return disconnect("closed by the sidecar");
    if(errno==EINTR)
      continue;
    if(errno==EAGAIN || errno==EWOULDBLOCK)
      break;
    return disconnect(std::strerror(errno));
    }

  size_t pos = 0;
  while(inBuf.size()-pos>=4) {
    auto*  p   = reinterpret_cast<const uint8_t*>(inBuf.data()+pos);
    size_t len = size_t(p[0]) | size_t(p[1])<<8 | size_t(p[2])<<16 | size_t(p[3])<<24;
    if(len>MaxFrame)
      return disconnect("frame too large");
    if(inBuf.size()-pos-4<len)
      break;
    handle(world, std::string_view(inBuf.data()+pos+4, len));
    if(fd<0)
      return;
    pos += 4+len;
    }
  inBuf.erase(inBuf.begin(), inBuf.begin()+ptrdiff_t(pos));
#else
  (void)world;
#endif
  }

void Sidecar::flush() {
#if defined(__linux__)
  while(!outBuf.empty()) {
    auto n = ::send(fd, outBuf.data(), outBuf.size(), MSG_NOSIGNAL);
    if(n>0) {
      outBuf.erase(0, size_t(n));
      continue;
      }
    if(n<0 && errno==EINTR)
      continue;
    if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK))
      return;
    return disconnect(n<0 ? std::strerror(errno) : "send failed");
    }
#endif
  }

void Sidecar::send(std::string_view json) {
  if(fd<0)
    return;
  auto len    = uint32_t(json.size());
  char hdr[4] = {char(len & 0xFF), char((len>>8) & 0xFF), char((len>>16) & 0xFF), char((len>>24) & 0xFF)};
  outBuf.append(hdr, sizeof(hdr));
  outBuf.append(json);
  }

void Sidecar::handle(World& world, std::string_view json) {
  rapidjson::Document doc;
  doc.Parse(json.data(), json.size());
  if(doc.HasParseError() || !doc.IsObject()) {
    note(true, "sidecar: malformed message");
    return;
    }

  auto type = str(doc,"type");
  if(type=="welcome")
    onWelcome(world,doc);
  else if(type=="intent")
    onIntent(world,doc);
  else if(type=="pass")
    onPass(world,doc);
  else
    note(true, "sidecar: unknown message type: "+std::string(type));
  }

void Sidecar::onWelcome(World& world, const rapidjson::Value& msg) {
  uint32_t protocol = 0;
  if(!u32(msg,"protocol",protocol) || protocol!=ProtocolVersion)
    return disconnect("protocol mismatch");

  controlled.clear();
  auto it = msg.FindMember("controlled");
  if(it!=msg.MemberEnd() && it->value.IsArray()) {
    for(auto& v:it->value.GetArray()) {
      if(!v.IsString())
        continue;
      std::string_view name(v.GetString(), v.GetStringLength());
      auto id = world.script().findSymbolIndex(name);
      if(id==size_t(-1)) {
        note(true, "sidecar: unknown NPC instance: "+std::string(name));
        continue;
        }
      auto& c = controlled[id];
      c.name = std::string(name);
      setMode(world,c,Mode::Idle);
      }
    }
  note(false, "sidecar: controlling "+std::to_string(controlled.size())+" NPC(s)");
  }

void Sidecar::onIntent(World& world, const rapidjson::Value& msg) {
  uint32_t intent = 0;
  if(!u32(msg,"intent",intent)) {
    note(true, "sidecar: intent without an id");
    return;
    }
  auto  npc    = str(msg,"npc");
  auto  reject = [&](std::string_view reason) { sendResult(world,npc,intent,"rejected",reason); };
  auto* c      = find(npc);
  if(c==nullptr)
    return reject("NPC is not controlled");

  uint32_t request = 0;
  if(u32(msg,"request",request) && (c->mode!=Mode::Awaiting || request!=c->request))
    return reject("stale request");

  auto it = msg.FindMember("actions");
  if(it==msg.MemberEnd() || !it->value.IsArray())
    return reject("no actions");

  std::vector<Action> actions;
  for(auto& v:it->value.GetArray()) {
    if(!v.IsObject())
      return reject("malformed action");
    Action a;
    auto kind = str(v,"action");
    if(kind=="go_to") {
      a.kind = Action::GoTo;
      a.text = std::string(str(v,"waypoint"));
      }
    else if(kind=="wait") {
      a.kind = Action::Wait;
      if(!u32(v,"ms",a.ms))
        return reject("wait without ms");
      }
    else if(kind=="say") {
      a.kind   = Action::Say;
      a.text   = std::string(str(v,"text"));
      a.target = std::string(str(v,"target"));
      }
    else {
      return reject("unknown action: "+std::string(kind));
      }
    actions.push_back(std::move(a));
    }

  if(c->hasPending)
    sendResult(world,c->name,c->pendingIntent,"rejected","superseded");
  // applied at the NPC's next decision point, when it is idle
  c->hasPending    = true;
  c->pendingIntent = intent;
  c->pending       = std::move(actions);
  }

void Sidecar::onPass(World& world, const rapidjson::Value& msg) {
  uint32_t request = 0;
  auto*    c       = find(str(msg,"npc"));
  if(c!=nullptr && u32(msg,"request",request) && c->mode==Mode::Awaiting && request==c->request)
    setMode(world,*c,Mode::Vanilla);
  }

Sidecar::Controlled* Sidecar::find(Npc& npc) {
  if(controlled.empty())
    return nullptr;
  auto it = controlled.find(size_t(npc.instanceSymbol()));
  if(it==controlled.end())
    return nullptr;
  // v0: of several NPCs sharing an instance, only the first one is controlled
  if(npc.world().findNpcByInstance(it->first)!=&npc)
    return nullptr;
  return &it->second;
  }

Sidecar::Controlled* Sidecar::find(std::string_view name) {
  for(auto& [id,c]:controlled)
    if(c.name==name)
      return &c;
  return nullptr;
  }

const char* Sidecar::modeName(Mode mode) {
  switch(mode) {
    case Mode::Idle:     return "idle";
    case Mode::Awaiting: return "awaiting";
    case Mode::Running:  return "running";
    case Mode::Vanilla:  return "vanilla";
    }
  return "unknown";
  }

void Sidecar::setMode(World& world, Controlled& c, Mode mode) {
  c.mode         = mode;
  c.modeSince    = world.time().toInt();
  c.modeTick     = world.tickCount();
  telemetryDirty = true;
  }

bool Sidecar::onDecisionPoint(Npc& npc, bool inRoutineState) {
  auto* c = find(npc);
  if(c==nullptr)
    return false;

  auto& world = npc.world();
  if(c->hasPending) {
    if(applyPending(npc,*c))
      return true;
    setMode(world,*c,Mode::Idle); // rejected: ask again
    }

  switch(c->mode) {
    case Mode::Awaiting:
      if(world.tickCount()-c->modeTick<DecideTimeout)
        return true;
      note(false, "sidecar: decision for "+c->name+" timed out");
      setMode(world,*c,Mode::Vanilla);
      return false;
    case Mode::Vanilla:
      if(inRoutineState)
        return false;
      break;
    case Mode::Running:
      sendResult(world,c->name,c->intent,"aborted",AbortQueueRanEmpty);
      break;
    case Mode::Idle:
      break;
    }
  requestDecision(npc,*c);
  return true;
  }

void Sidecar::requestDecision(Npc& npc, Controlled& c) {
  auto& world = npc.world();
  c.request = nextRequest++;
  setMode(world,c,Mode::Awaiting);

  rapidjson::StringBuffer buf;
  Writer w(buf);
  w.StartObject();
  w.Key("type");    w.String("decide");
  w.Key("request"); w.Uint(c.request);
  w.Key("npc");     writeText(w,c.name);
  w.Key("time");    w.Int64(world.time().toInt());

  w.Key("snapshot");
  w.StartObject();
  auto* wp = world.findWayPoint(npc.position());
  if(wp!=nullptr) {
    w.Key("waypoint");
    writeText(w,wp->name);
    }
  w.Key("waypoints");
  w.StartArray();
  for(auto& n:neighbours(npc,wp)) {
    w.StartObject();
    w.Key("name");     writeText(w,n.point->name);
    w.Key("distance"); w.Double(double(n.distance));
    w.EndObject();
    }
  w.EndArray();
  if(auto* pl = world.player()) {
    w.Key("player_distance");
    w.Double(double(std::sqrt(npc.qDistTo(*pl))));
    }
  w.EndObject();

  w.EndObject();
  send({buf.GetString(), buf.GetSize()});
  }

bool Sidecar::applyPending(Npc& npc, Controlled& c) {
  c.hasPending = false;
  auto& world  = npc.world();
  auto  reject = [&](std::string_view reason) {
    sendResult(world,c.name,c.pendingIntent,"rejected",reason);
    return false;
    };

  std::vector<AiQueue::AiAction> queue;
  bool firstGoTo = true;
  for(auto& a:c.pending) {
    switch(a.kind) {
      case Action::GoTo: {
        auto* wp = world.findPoint(a.text,false);
        if(wp==nullptr)
          return reject("unknown waypoint "+a.text);
        // later go_to steps start elsewhere; v0 checks only the first one
        if(firstGoTo && !isReachable(world,npc,*wp))
          return reject("unreachable waypoint "+a.text);
        firstGoTo = false;
        queue.push_back(AiQueue::aiGoToPoint(*wp));
        break;
        }
      case Action::Wait:
        queue.push_back(AiQueue::aiWait(a.ms));
        break;
      case Action::Say: {
        if(!a.target.empty()) {
          auto* target = findNpc(world,a.target);
          if(target==nullptr)
            return reject("unknown target "+a.target);
          queue.push_back(AiQueue::aiTurnToNpc(target));
          }
        auto line = std::string(npc.displayName()) + ": " + fromUtf8(a.text);
        queue.push_back(AiQueue::aiPrintScreen(SayTime,SayFont,-1,SayPosY,line));
        break;
        }
      }
    }

  AiQueue::AiAction done;
  done.act = AI_SidecarDone;
  done.i0  = int(c.pendingIntent);
  queue.push_back(std::move(done));
  for(auto& a:queue)
    npc.aiPush(std::move(a));

  c.intent   = c.pendingIntent;
  c.deadline = world.tickCount() + RunTimeout;
  c.pending.clear();
  setMode(world,c,Mode::Running);
  return true;
  }

void Sidecar::onIntentDone(Npc& npc, uint32_t intent) {
  auto* c = find(npc);
  if(c==nullptr || c->mode!=Mode::Running || c->intent!=intent)
    return;
  setMode(npc.world(),*c,Mode::Idle);
  sendResult(npc.world(),c->name,intent,"done");
  }

void Sidecar::onAiQueueCleared(Npc& npc) {
  auto* c = find(npc);
  if(c==nullptr || c->mode!=Mode::Running)
    return;
  setMode(npc.world(),*c,Mode::Idle);
  sendResult(npc.world(),c->name,c->intent,"aborted",AbortQueueCleared);
  }

void Sidecar::checkDeadlines(World& world) {
  for(auto& [id,c]:controlled) {
    if(c.mode!=Mode::Running || world.tickCount()<c.deadline)
      continue;
    setMode(world,c,Mode::Idle);
    sendResult(world,c.name,c.intent,"aborted",AbortTimeout);
    // no longer Running, so onAiQueueCleared stays silent
    if(auto* npc = world.findNpcByInstance(id))
      npc->clearAiQueue();
    }
  }

void Sidecar::sendTelemetry(World& world) {
  auto* pl = world.player();

  std::vector<std::pair<size_t,const Controlled*>> npcs;
  for(auto& [id,c]:controlled)
    npcs.emplace_back(id,&c);
  std::sort(npcs.begin(), npcs.end(), [](const auto& a, const auto& b) {
    return a.second->name<b.second->name;
    });

  rapidjson::StringBuffer buf;
  Writer w(buf);
  w.StartObject();
  w.Key("type"); w.String("telemetry");
  w.Key("time"); w.Int64(world.time().toInt());
  w.Key("tick"); w.Uint64(world.tickCount());
  if(pl!=nullptr) {
    w.Key("player");
    w.StartObject();
    writePosition(w,*pl);
    if(auto* wp = world.findWayPoint(pl->position()))
      writeName(w,"waypoint",wp->name);
    w.EndObject();
    }

  w.Key("npcs");
  w.StartArray();
  for(auto& [id,c]:npcs) {
    w.StartObject();
    w.Key("npc");        writeText(w,c->name);
    w.Key("mode");       w.String(modeName(c->mode));
    w.Key("mode_since"); w.Int64(c->modeSince);
    if(c->mode==Mode::Awaiting) {
      w.Key("request");
      w.Uint(c->request);
      }
    if(c->mode==Mode::Running) {
      w.Key("intent");
      w.Uint(c->intent);
      }

    if(auto* npc = world.findNpcByInstance(id)) {
      w.Key("world");
      w.StartObject();
      w.Key("ai"); w.String(aiName(npc->processPolicy()));
      writeName(w,"state",  symbolName(world,npc->aiState.funcIni));
      writeName(w,"routine",symbolName(world,npc->currentRoutine().callback));
      w.Key("body_state"); w.String(bodyStateName(npc->bodyStateMasked()));

      auto& queue = npc->aiQueue.aiActions;
      w.Key("queue");
      w.StartArray();
      for(size_t i=0; i<queue.size() && i<MaxQueueShown; ++i)
        w.String(actionName(queue[i].act));
      w.EndArray();
      w.Key("queue_len"); w.Uint(uint32_t(queue.size()));

      if(npc->go2.flag!=Npc::GT_No && npc->go2.wp!=nullptr)
        writeName(w,"next_waypoint",npc->go2.wp->name);
      if(auto* last = npc->wayPath.last())
        writeName(w,"destination",last->name);
      if(auto* mob = npc->interactive())
        writeName(w,"using",mob->schemeName());
      if(auto* wp = world.findWayPoint(npc->position()))
        writeName(w,"waypoint",wp->name);
      writePosition(w,*npc);
      if(pl!=nullptr) {
        w.Key("player_distance");
        w.Double(double(std::sqrt(npc->qDistTo(*pl))));
        }
      w.EndObject();
      }
    w.EndObject();
    }
  w.EndArray();
  w.EndObject();
  send({buf.GetString(), buf.GetSize()});
  }

void Sidecar::note(bool error, std::string_view text) {
  if(error)
    Log::e(text); else
    Log::i(text);
  if(fd<0)
    return;

  rapidjson::StringBuffer buf;
  Writer w(buf);
  w.StartObject();
  w.Key("type");  w.String("log");
  w.Key("level"); w.String(error ? "error" : "info");
  w.Key("text");  writeText(w,text);
  w.EndObject();
  send({buf.GetString(), buf.GetSize()});
  }

void Sidecar::onPerception(Npc& npc, Npc& other, PercType perc) {
  auto* c = find(npc);
  if(c==nullptr)
    return;

  rapidjson::StringBuffer buf;
  Writer w(buf);
  w.StartObject();
  w.Key("type"); w.String("perception");
  w.Key("npc");  writeText(w,c->name);
  w.Key("time"); w.Int64(npc.world().time().toInt());
  w.Key("kind"); w.String(perceptionName(perc));
  auto otherName = instanceName(other);
  if(!otherName.empty()) {
    w.Key("other");
    writeText(w,otherName);
    }
  w.EndObject();
  send({buf.GetString(), buf.GetSize()});
  }

void Sidecar::sendResult(World& world, std::string_view npc, uint32_t intent,
                         std::string_view status, std::string_view reason) {
  rapidjson::StringBuffer buf;
  Writer w(buf);
  w.StartObject();
  w.Key("type");   w.String("result");
  w.Key("intent"); w.Uint(intent);
  w.Key("npc");    writeText(w,npc);
  w.Key("time");   w.Int64(world.time().toInt());
  w.Key("outcome");
  w.StartObject();
  w.Key("status"); w.String(status.data(), rapidjson::SizeType(status.size()));
  if(!reason.empty()) {
    w.Key("reason");
    writeText(w,reason);
    Log::i("sidecar: intent ", int(intent), " ", status, ": ", reason);
    }
  w.EndObject();
  w.EndObject();
  send({buf.GetString(), buf.GetSize()});
  }
