#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <rapidjson/fwd.h>

#include "game/constants.h"

class Npc;
class World;

/**
 * Client of the sleeper sidecar: an external process that drives selected NPCs.
 * Inactive unless the game is started with `-sidecar <socket path>`.
 * Protocol: docs/protocol.md in the sleeper repository.
 */
class Sidecar final {
  public:
    static Sidecar& inst();

    // Main thread, once per game tick, before the world ticks.
    void tick(World& world);

    // Cheap check for hot paths: does the sidecar control any NPC at all?
    bool isActive() const { return !controlled.empty(); }

    // NPC has an empty AI queue and no AI state, or only its routine state.
    // Returns true when the sidecar handles the NPC, so the vanilla routine must not run.
    bool onDecisionPoint(Npc& npc, bool inRoutineState);
    void onIntentDone(Npc& npc, uint32_t intent);
    void onAiQueueCleared(Npc& npc);
    void onPerception(Npc& npc, Npc& other, PercType perc);

  private:
    using Clock = std::chrono::steady_clock;

    Sidecar();
    ~Sidecar();

    enum class Mode : uint8_t {
      Idle,     // ask at the next decision point
      Awaiting, // `decide` sent, waiting for an intent or pass
      Running,  // intent pushed onto the AI queue
      Vanilla,  // pass or timeout: vanilla routine until its state ends
      };

    struct Action {
      enum Kind : uint8_t { GoTo, Wait, Say };
      Kind        kind = Wait;
      uint32_t    ms   = 0;
      std::string text;   // waypoint for GoTo, UTF-8 line for Say
      std::string target; // Say only, optional
      };

    struct Controlled {
      std::string         name;
      Mode                mode    = Mode::Idle;
      uint32_t            request = 0;
      uint32_t            intent  = 0;
      Clock::time_point   since;

      bool                hasPending    = false;
      uint32_t            pendingIntent = 0;
      std::vector<Action> pending;
      };

    void        connect(World& world);
    void        disconnect(std::string_view reason);
    void        receive(World& world);
    void        flush();
    // Queues a frame; it is written at the end of the next `tick`, so callers
    // never see a disconnect (and a cleared `controlled`) mid-call.
    void        send(std::string_view json);

    void        handle(World& world, std::string_view json);
    void        onWelcome(World& world, const rapidjson::Value& msg);
    void        onIntent(const rapidjson::Value& msg);
    void        onPass(const rapidjson::Value& msg);

    Controlled* find(Npc& npc);
    Controlled* find(std::string_view name);
    void        requestDecision(Npc& npc, Controlled& c);
    bool        applyPending(Npc& npc, Controlled& c);
    void        sendResult(uint32_t intent, std::string_view status, std::string_view reason = {});

    std::string                             socketPath;
    int                                     fd = -1;
    Clock::time_point                       nextConnect;
    std::vector<char>                       inBuf;
    std::string                             outBuf;

    const World*                            lastWorld   = nullptr;
    uint32_t                                nextRequest = 1;
    // keyed by the NPC's instance symbol index in the current script
    std::unordered_map<size_t, Controlled>  controlled;
  };
