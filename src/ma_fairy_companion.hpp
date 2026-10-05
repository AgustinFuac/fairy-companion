#pragma once

#include "mods/svc/actor.h"
#include "f_op/f_op_actor.h"
#include "SSystem/SComponent/c_phase.h"
#include "Z2AudioLib/Z2Creature.h"

// Forward declarations
class mDoExt_McaMorfSO;
class J3DModel;

// HUD Hooks / Control
void maFairyHud_init();
void maFairyHud_shutdown();

// Sonidos de entrar/salir (implementado en mod.cpp)
void maFairySfx_play(bool hiding);   // true = se guarda en Link, false = sale

#define MFAIRYC_NAME "mFairyC"

// Color identifier (indices into fairy tint table)
enum FairyColor_e {
    FAIRY_COLOR_BLUE   = 0,  // Navi
    FAIRY_COLOR_ORANGE = 1,  // Tatl
    FAIRY_COLOR_PURPLE = 2,  // Tael
    FAIRY_COLOR_GREEN  = 3,  // Extra
};

// Ability trigger button (D-Pad Left or Right to avoid conflict with native actions)
enum FairyButton_e {
    FAIRY_BUTTON_LEFT  = 0,
    FAIRY_BUTTON_RIGHT = 1,
};

class maFairyCompanion_c : public fopAc_ac_c {
public:
    // --- Model & Audio ---
    mDoExt_McaMorfSO* mpModelMorf;
    Z2Creature        mSound;

    // --- Flight & Follow Dynamics ---
    f32 mOrbitAngle;    // Target orbit angle / base phase for pulse
    f32 mSwayPhase;     // Lateral sway phase behind Link
    f32 mBobPhase;      // Vertical bob phase

    // --- Particle Systems ---
    u32 mParticleId;    // Soft trail emitter ID (0x72F)
    u32 mSparkleId;     // Sparkle emitter ID (0x731)
    u32 mAuraId;        // Body aura emitter ID (0x730)
    u8  mParticleTimer; // Trail timer
    u8  mSparkleTimer;  // Sparkle timer
    u8  mAuraTimer;     // Aura timer
    u8  mColorId;       // Active color index

    // --- Combat / Ability State ---
    u32  mFrozenId;     // Frozen/Slowed actor ID (k_noActorId if none)
    s16  mFrozenTimer;  // Remaining effect frames
    bool mAffectSlow;   // true = Navi (slowdown), false = Tatl (freeze)

    virtual ~maFairyCompanion_c();

    cPhs_Step create();
    int       CreateHeap();
    int       Delete();
    int       Execute();
    int       Draw();
    void      setBaseMtx();

    // Material tinting (applies color to model TEV registers after environment pass)
    void applyFairyTint(J3DModel* i_model);
    void releaseFrozen();

    // --- Global Configuration (Shared across respawns) ---
    static u8   sSelectedColor;
    static void setSelectedColor(u8 colorId);
    static u8   getSelectedColor() { return sSelectedColor; }

    static bool sLightEnabled;
    static void setLightEnabled(bool enabled) { sLightEnabled = enabled; }
    static bool getLightEnabled() { return sLightEnabled; }

    static u8   sActionButton;
    static void setActionButton(u8 button) {
        sActionButton = (button <= FAIRY_BUTTON_RIGHT) ? button : FAIRY_BUTTON_LEFT;
    }
    static u8   getActionButton() { return sActionButton; }

    // --- Actor Profile & Registration ---
    static int createHeapCallBack(fopAc_ac_c*);

    static s16                    sProcName;
    static ActorHandle            sActorHandle;
    static const ActorProfileDesc sProfile;

    // --- HUD Status ---
    static bool sAbilityReady;
    static u32  sExecTick;   // sube en cada Execute (detecta pausa)

};