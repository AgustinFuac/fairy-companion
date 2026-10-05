#include "ma_fairy_companion.hpp"

#include <cmath>

// Dolphin / Hardware
#include <dolphin/gx.h>
#include <dolphin/gx/GXVert.h>

// JSystem
#include "JSystem/J2DGraph/J2DGrafContext.h"
#include "JSystem/J2DGraph/J2DPicture.h"
#include "JSystem/J2DGraph/J2DScreen.h"
#include "JSystem/J3DGraphAnimator/J3DModelData.h"
#include "JSystem/J3DGraphBase/J3DMaterial.h"

// Game Engine
#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_player.h"
#include "d/d_attention.h"
#include "d/d_bomb.h"
#include "d/d_com_inf_game.h"
#include "d/d_kankyo.h"
#include "d/d_meter2_draw.h"
#include "f_op/f_op_actor_mng.h"
#include "m_Do/m_Do_controller_pad.h"
#include "m_Do/m_Do_ext.h"
#include "m_Do/m_Do_mtx.h"

// Dusklight SDK
#include "mods/svc/hook.hpp"
#include "mods/svc/resource.h"

// ---------------------------------------------------------------------------
// Constants & Configuration
// ---------------------------------------------------------------------------

// Solid heap size for model + animation (matches daObjYOUSEI_c requirements)
static constexpr u32 k_heapSize = 0x1100;

// Default floor color for dKy_tevstr_init
static constexpr u8 k_defaultFloorCol = 0xFF;

// Actor ID sentinel for no actor
static constexpr u32 k_noActorId = 0xFFFFFFFF;

// Ability durations (logic frames at 30 fps)
static constexpr s16 k_tatlFreezeFrames = 150; // 5 seconds
static constexpr s16 k_naviSlowFrames = 240; // 8 seconds
static constexpr int k_slowRunEvery = 3;   // Runs 1 out of every N frames when slowed

// Particle update intervals
static constexpr u8 k_particleInterval = 8;
static constexpr u8 k_auraInterval = 1;

// Material register index for fairy tint
static constexpr u32 k_fairyTintReg = 1;

// Math constants
static constexpr f32 k_pi = 3.14159265358979323846f;
static constexpr f32 k_twoPi = 2.0f * k_pi;
static constexpr f32 k_angleToRad = k_pi / 32768.0f; // Converts s16 engine angle to radians

// ---------------------------------------------------------------------------
// Hide & Seek (fairy tucks into Link now and then, like Navi)
// ---------------------------------------------------------------------------

static bool s_wantHidden = false; // Desired state (schedule)
static s32  s_stateTimer = -1;    // Frames until the next state flip (-1 = roll a new one)
static f32  s_hideBlend = 0.0f;  // 0 = out flying, 1 = fully tucked into Link
static f32  s_baseScale = 0.0f;  // Normal model scale, captured on first Execute

static constexpr f32 k_tuckHeight = 130.0f; // Height above Link's feet where she tucks in
static constexpr f32 k_hideRate = 0.035f; // Blend speed when going into Link
static constexpr f32 k_emergeRate = 0.12f;  // Blend speed when coming out for a target

// Durations in Execute frames (30 frames = 1 second)
static constexpr s32 k_awakeMin = 450;  // ~15 s visible at least def:600
static constexpr s32 k_awakeMax = 900; // ~30 s visible at most def:1800
static constexpr s32 k_hiddenMin = 180;  // ~6 s hidden at least
static constexpr s32 k_hiddenMax = 300;  // ~10 s hidden at most

static u32 s_rndState = 0x1234ABCD;
static s32 fairy_randRange(s32 lo, s32 hi) {
    s_rndState = s_rndState * 1664525u + 1013904223u + maFairyCompanion_c::sExecTick;
    return lo + static_cast<s32>((s_rndState >> 8) % static_cast<u32>(hi - lo + 1));
}

// ---------------------------------------------------------------------------
// Fairy Color Palette
// ---------------------------------------------------------------------------

struct FairyTint_s {
    u8 bodyR, bodyG, bodyB; // Material TEV registers and ambient light
    u8 prmR, prmG, prmB;  // Particle trail & sparkle primary color
    u8 envR, envG, envB;  // Particle trail & sparkle environment color
    u8 auraR, auraG, auraB; // Body aura glow color (0x730)
    u8 auraAlpha;           // Default aura opacity
};

static const FairyTint_s k_fairyTints[] = {
    // FAIRY_COLOR_BLUE (Navi)
    {   0, 117, 235,   80, 180, 255,   20,  60, 200,   70, 170, 255, 140 },
    // FAIRY_COLOR_ORANGE (Tatl)
    { 200, 150,  20,  255, 190,  70,  200,  90,  10,  255, 175,  60, 140 },
    // FAIRY_COLOR_PURPLE (Tael)
    { 204,   0, 240,  102,   0,  80,  100,  20, 200,  175,  90, 255, 140 },
    // FAIRY_COLOR_GREEN (Extra)
    {  50, 255,  80,  110, 255, 140,   20, 180,  50,   90, 255, 120, 140 },
};

static constexpr u8 k_fairyTintCount = static_cast<u8>(sizeof(k_fairyTints) / sizeof(k_fairyTints[0]));

static inline const FairyTint_s& getFairyTint(u8 colorId) {
    if (colorId >= k_fairyTintCount) {
        colorId = FAIRY_COLOR_BLUE;
    }
    return k_fairyTints[colorId];
}

static inline void wrapAngle(f32& angle) {
    if (angle > k_twoPi) {
        angle -= k_twoPi;
    }
}

// ---------------------------------------------------------------------------
// Static Member Initialization
// ---------------------------------------------------------------------------

u8          maFairyCompanion_c::sSelectedColor = FAIRY_COLOR_BLUE;
bool        maFairyCompanion_c::sLightEnabled = true;
u8          maFairyCompanion_c::sActionButton = FAIRY_BUTTON_LEFT;
bool        maFairyCompanion_c::sAbilityReady = false;
u32         maFairyCompanion_c::sExecTick = 0;
s16         maFairyCompanion_c::sProcName = -1;
ActorHandle maFairyCompanion_c::sActorHandle = -1;

void maFairyCompanion_c::setSelectedColor(u8 colorId) {
    sSelectedColor = (colorId < k_fairyTintCount) ? colorId : FAIRY_COLOR_BLUE;
}

// ---------------------------------------------------------------------------
// Lifecycle & Resource Management
// ---------------------------------------------------------------------------

maFairyCompanion_c::~maFairyCompanion_c() {
    // "Always" archive is resident throughout the game; no resource unloading needed.
}

cPhs_Step maFairyCompanion_c::create() {
    fopAcM_ct(this, maFairyCompanion_c);

    if (!fopAcM_entrySolidHeap(this, createHeapCallBack, k_heapSize)) {
        return cPhs_ERROR_e;
    }

    dKy_tevstr_init(&tevStr, fopAcM_GetRoomNo(this), k_defaultFloorCol);

    fopAcM_SetMtx(this, mpModelMorf->getModel()->getBaseTRMtx());
    fopAcM_SetMin(this, -50.0f, -50.0f, -50.0f);
    fopAcM_SetMax(this, 50.0f, 50.0f, 50.0f);

    mSound.init(&current.pos, &eyePos, 3, 1);

    mOrbitAngle = 0.0f;
    mSwayPhase = 0.0f;
    mBobPhase = 1.5f; // Offset phase so bob does not sync with sway
    mColorId = sSelectedColor;
    mParticleId = 0;
    mParticleTimer = 0;
    mSparkleId = 0;
    mSparkleTimer = 0;
    mAuraId = 0;
    mAuraTimer = 0;
    mFrozenId = k_noActorId;
    mFrozenTimer = 0;
    mAffectSlow = false;

    Execute();
    return cPhs_COMPLEATE_e;
}

int maFairyCompanion_c::CreateHeap() {
    // Load fairy model (0x21) and flight animation (0xF) from resident "Always" archive
    void* modelData = dComIfG_getObjectRes("Always", 0x21);
    if (modelData == NULL) {
        return 0;
    }

    mpModelMorf = JKR_NEW mDoExt_McaMorfSO(
        static_cast<J3DModelData*>(modelData), NULL, NULL,
        static_cast<J3DAnmTransform*>(dComIfG_getObjectRes("Always", 0xF)),
        2, 0.4f, 0, -1, &mSound, 0x80000, 0x11000084);

    return mpModelMorf != NULL;
}

int maFairyCompanion_c::createHeapCallBack(fopAc_ac_c* i_this) {
    return static_cast<maFairyCompanion_c*>(i_this)->CreateHeap();
}

int maFairyCompanion_c::Delete() {
    releaseFrozen();
    sAbilityReady = false;

    // Reset hide & seek state for the next respawn
    s_wantHidden = false;
    s_stateTimer = -1;
    s_hideBlend = 0.0f;
    s_baseScale = 0.0f;

    if (heap != NULL && mpModelMorf != NULL) {
        mpModelMorf->stopZelAnime();
    }
    this->~maFairyCompanion_c();
    return 1;
}

void maFairyCompanion_c::releaseFrozen() {
    if (mFrozenId != k_noActorId) {
        fopAc_ac_c* frozen = fopAcM_SearchByID(mFrozenId);
        if (frozen != NULL) {
            fpcM_PauseDisable(frozen, 1);
        }
        mFrozenId = k_noActorId;
    }
    mFrozenTimer = 0;
    mAffectSlow = false;
}

// ---------------------------------------------------------------------------
// Execution & Movement
// ---------------------------------------------------------------------------

int maFairyCompanion_c::Execute() {
    sExecTick++; // Heartbeat used by the HUD to detect pause
    mColorId = sSelectedColor;

    // Remember the normal model scale (before hide shrinking touches it)
    if (s_baseScale <= 0.0f && scale.x > 0.01f) {
        s_baseScale = scale.x;
    }

    daPy_py_c* player = daPy_getPlayerActorClass();
    daAlink_c* alink = daAlink_getAlinkActorClass();

    // Check attention lock-on target
    dAttention_c* attention = dComIfGp_getAttention();
    fopAc_ac_c* lockTarget = (attention != NULL && attention->Lockon())
        ? attention->LockonTarget(0) : NULL;

    const bool lockedOnEnemy = (lockTarget != NULL)
        && (fopAcM_GetGroup(lockTarget) == fopAc_ENEMY_e);

    sAbilityReady = lockedOnEnemy;

    // Hide & seek schedule: any lock-on target brings her out immediately
    const bool prevWantHidden = s_wantHidden;
    if (lockTarget != NULL) {
        s_wantHidden = false;
        s_stateTimer = -1;
    }
    else {
        if (s_stateTimer < 0) {
            s_stateTimer = s_wantHidden ? fairy_randRange(k_hiddenMin, k_hiddenMax)
                : fairy_randRange(k_awakeMin, k_awakeMax);
        }
        if (--s_stateTimer <= 0) {
            s_wantHidden = !s_wantHidden;
            s_stateTimer = -1;
        }
    }

    if (s_wantHidden != prevWantHidden) {       // <-- NUEVO
        maFairySfx_play(s_wantHidden);
    }

    // Move the blend toward its goal and shrink the model accordingly
    {
        const f32 goal = s_wantHidden ? 1.0f : 0.0f;
        const f32 rate = (lockTarget != NULL) ? k_emergeRate : k_hideRate;
        if (s_hideBlend < goal) {
            s_hideBlend = (s_hideBlend + rate > goal) ? goal : (s_hideBlend + rate);
        }
        else if (s_hideBlend > goal) {
            s_hideBlend = (s_hideBlend - rate < goal) ? goal : (s_hideBlend - rate);
        }

        f32 s = s_baseScale * (1.0f - s_hideBlend);
        if (s < 0.001f) {
            s = 0.001f;
        }
        scale.x = s;
        scale.y = s;
        scale.z = s;
    }

    if (lockTarget != NULL) {
        // Orbit around the locked target
        mOrbitAngle += 0.06f;
        wrapAngle(mOrbitAngle);

        constexpr f32 orbitRadius = 50.0f;
        constexpr f32 orbitHeight = 100.0f;

        const cXyz& centerPos = lockTarget->current.pos;
        cXyz hoverPos;
        hoverPos.x = centerPos.x + orbitRadius * std::sin(mOrbitAngle);
        hoverPos.y = centerPos.y + orbitHeight;
        hoverPos.z = centerPos.z + orbitRadius * std::cos(mOrbitAngle);

        constexpr f32 orbitFollowSpeed = 0.15f;
        current.pos.x += (hoverPos.x - current.pos.x) * orbitFollowSpeed;
        current.pos.y += (hoverPos.y - current.pos.y) * orbitFollowSpeed;
        current.pos.z += (hoverPos.z - current.pos.z) * orbitFollowSpeed;

        cXyz diff = centerPos - current.pos;
        shape_angle.y = cM_atan2s(diff.x, diff.z);
    }
    else if (player != NULL) {
        // Follow Link from behind with lateral sway and vertical bobbing
        const f32 facingRad = player->current.angle.y * k_angleToRad;
        const f32 behindRad = facingRad + k_pi;

        const f32 sinBehind = std::sin(behindRad);
        const f32 cosBehind = std::cos(behindRad);

        constexpr f32 followDist = 25.0f;
        f32 targetX = player->current.pos.x + followDist * sinBehind;
        f32 targetZ = player->current.pos.z + followDist * cosBehind;

        // Sway lateral: perpendicular using trig identity
        // sin(theta + pi/2) = cos(theta), cos(theta + pi/2) = -sin(theta)
        mSwayPhase += 0.025f;
        wrapAngle(mSwayPhase);
        const f32 sway = 70.0f * std::sin(mSwayPhase);
        targetX += sway * cosBehind;
        targetZ -= sway * sinBehind;

        // Bob vertical
        mBobPhase += 0.035f;
        wrapAngle(mBobPhase);
        const f32 bob = 27.0f * std::sin(mBobPhase);

        // Lower height offset when riding horseback / boar
        const f32 heightOffset = (alink != NULL && alink->checkReinRide()) ? 80.0f : 160.0f;

        // Blend the normal flight target with the tuck-in point on Link
        const f32 flyY = player->current.pos.y + heightOffset + bob;
        const f32 tuckY = player->current.pos.y + k_tuckHeight;
        const f32 goalX = targetX + (player->current.pos.x - targetX) * s_hideBlend;
        const f32 goalZ = targetZ + (player->current.pos.z - targetZ) * s_hideBlend;
        const f32 goalY = flyY + (tuckY - flyY) * s_hideBlend;

        const f32 followSpeed = 0.12f + 0.08f * s_hideBlend; // Flies a bit faster when tucking in
        current.pos.x += (goalX - current.pos.x) * followSpeed;
        current.pos.y += (goalY - current.pos.y) * followSpeed;
        current.pos.z += (goalZ - current.pos.z) * followSpeed;

        shape_angle.y = fopAcM_searchPlayerAngleY(this);
    }

    // Update active freeze/slow state on targeted enemy
    if (mFrozenId != k_noActorId) {
        fopAc_ac_c* affected = fopAcM_SearchByID(mFrozenId);
        if (affected == NULL || --mFrozenTimer <= 0) {
            releaseFrozen();
        }
        else {
            const bool shouldPause = mAffectSlow ? ((mFrozenTimer % k_slowRunEvery) != 0) : true;
            if (shouldPause) {
                if (!fpcM_IsPause(affected, 1)) {
                    fpcM_PauseEnable(affected, 1);
                }
            }
            else {
                if (fpcM_IsPause(affected, 1)) {
                    fpcM_PauseDisable(affected, 1);
                }
            }
        }
    }

    // Fairy ability trigger on button press
    bool actionPressed = false;
    switch (sActionButton) {
    case FAIRY_BUTTON_RIGHT:
        actionPressed = (mDoCPd_c::getTrigRight(PAD_1) != 0);
        break;
    case FAIRY_BUTTON_LEFT:
    default:
        actionPressed = (mDoCPd_c::getTrigLeft(PAD_1) != 0);
        break;
    }

    if (lockedOnEnemy && actionPressed) {
        switch (mColorId) {
        case FAIRY_COLOR_BLUE:      // Navi: Slow enemy for 8s
        case FAIRY_COLOR_ORANGE: {  // Tatl: Freeze enemy for 5s
            if (mFrozenId == k_noActorId) {
                mAffectSlow = (mColorId == FAIRY_COLOR_BLUE);
                mFrozenId = fopAcM_GetID(lockTarget);
                mFrozenTimer = mAffectSlow ? k_naviSlowFrames : k_tatlFreezeFrames;
                fpcM_PauseEnable(lockTarget, 1);
            }
            break;
        }
        case FAIRY_COLOR_PURPLE: {  // Tael: Bomb explosion
            cXyz bombPos = lockTarget->current.pos;
            dBomb_c::createNormalBombExplode(&bombPos);
            break;
        }
        default:
            break;
        }
    }

    // Refresh ambient lighting
    tevStr.room_no = fopAcM_GetRoomNo(this);
    dKy_setLight_nowroom_actor(&tevStr);

    const FairyTint_s& tint = getFairyTint(mColorId);

    // Omnidirectional dynamic fairy light (off while she is tucked into Link)
    if (sLightEnabled && s_hideBlend < 0.5f) {
        GXColor lightCol = { tint.bodyR, tint.bodyG, tint.bodyB, 0xFF };
        dKy_WolfEyeLight_set(&current.pos, 0.0f, 0.0f, 50.0f, &lightCol, 1.0f, 0, 3);
    }

    // Particle Trail & Sparkles (stop once she is mostly hidden)
    const GXColor prmColor = { tint.prmR, tint.prmG, tint.prmB, 0xFF };
    const GXColor envColor = { tint.envR, tint.envG, tint.envB, 0xFF };

    if (s_hideBlend < 0.5f && ++mParticleTimer >= k_particleInterval) {
        mParticleTimer = 0;
        mParticleId = dComIfGp_particle_set(
            mParticleId, 0x72F, &current.pos, &tevStr, &shape_angle,
            NULL, 0xFF, NULL, -1, &prmColor, &envColor, NULL);
    }

    if (s_hideBlend < 0.5f && ++mSparkleTimer >= k_particleInterval) {
        mSparkleTimer = 0;
        mSparkleId = dComIfGp_particle_set(
            mSparkleId, 0x731, &current.pos, &tevStr, &shape_angle,
            NULL, 0xFF, NULL, -1, &prmColor, &envColor, NULL);
    }

    // Body aura glow (0x730)
    if (s_hideBlend < 0.5f && ++mAuraTimer >= k_auraInterval) {
        mAuraTimer = 0;
        const GXColor auraCol = { tint.auraR, tint.auraG, tint.auraB, 0xFF };
        const u8 auraAlpha = lockedOnEnemy ? 255 : tint.auraAlpha;

        mAuraId = dComIfGp_particle_set(
            mAuraId, 0x730, &current.pos, &tevStr, &shape_angle,
            NULL, auraAlpha, NULL, -1, &auraCol, &auraCol, NULL);
    }

    // Advance wing animation
    mpModelMorf->play(0, dComIfGp_getReverb(fopAcM_GetRoomNo(this)));

    eyePos = attention_info.position = current.pos;
    attention_info.flags = 0;

    setBaseMtx();
    return 1;
}

void maFairyCompanion_c::setBaseMtx() {
    mDoMtx_stack_c::transS(current.pos);
    mDoMtx_stack_c::ZXYrotM(shape_angle);
    mDoMtx_stack_c::scaleM(scale);
    mpModelMorf->getModel()->setBaseTRMtx(mDoMtx_stack_c::get());
    mpModelMorf->modelCalc();
}

// ---------------------------------------------------------------------------
// Rendering & Material Tinting
// ---------------------------------------------------------------------------

void maFairyCompanion_c::applyFairyTint(J3DModel* i_model) {
    const FairyTint_s& tint = getFairyTint(mColorId);
    J3DModelData* modelData = i_model->getModelData();
    const u16 matNum = modelData->getMaterialNum();

    for (u16 i = 0; i < matNum; i++) {
        J3DMaterial* mat = modelData->getMaterialNodePointer(i);
        if (mat == NULL) continue;

        J3DGXColorS10* c = mat->getTevColor(k_fairyTintReg);
        if (c != NULL) {
            c->r = static_cast<s16>(tint.bodyR);
            c->g = static_cast<s16>(tint.bodyG);
            c->b = static_cast<s16>(tint.bodyB);
        }
    }
}

int maFairyCompanion_c::Draw() {
    // Fully tucked into Link: nothing to draw
    if (s_hideBlend >= 0.99f) {
        return 1;
    }

    J3DModel* model = mpModelMorf->getModel();

    // 1) Environment system sets up lighting structure
    g_env_light.settingTevStruct(0, &current.pos, &tevStr);

    // 2) Write environment lighting to material registers
    g_env_light.setLightTevColorType_MAJI(model, &tevStr);

    // 3) Apply our custom fairy hue over register 1
    applyFairyTint(model);

    mpModelMorf->entryDL();
    return 1;
}

// ---------------------------------------------------------------------------
// Actor Profile Callbacks
// ---------------------------------------------------------------------------

static cPhs_Step maFairyCompanion_Create(void* i_this) {
    return static_cast<maFairyCompanion_c*>(i_this)->create();
}

static int maFairyCompanion_Delete(void* i_this) {
    return static_cast<maFairyCompanion_c*>(i_this)->Delete();
}

static int maFairyCompanion_Execute(void* i_this) {
    return static_cast<maFairyCompanion_c*>(i_this)->Execute();
}

static int maFairyCompanion_Draw(void* i_this) {
    return static_cast<maFairyCompanion_c*>(i_this)->Draw();
}

static int maFairyCompanion_IsDelete(void*) {
    return 1;
}

const ActorProfileDesc maFairyCompanion_c::sProfile = {
    .name = MFAIRYC_NAME,
    .priority_group = 7,
    .process_size = sizeof(maFairyCompanion_c),
    .draw_priority = fpcDwPi_OBJ_LBOX_e,
    .status = fopAcStts_UNK_0x40000_e | fopAcStts_UNK_0x4000_e | fopAcStts_CULL_e,
    .group = fopAc_ACTOR_e,
    .cull_type = fopAc_CULLBOX_CUSTOM_e,
    .create_function = maFairyCompanion_Create,
    .delete_function = maFairyCompanion_Delete,
    .execute_function = maFairyCompanion_Execute,
    .is_delete_function = maFairyCompanion_IsDelete,
    .draw_function = maFairyCompanion_Draw,
};

// ---------------------------------------------------------------------------
// HUD Icon & Meter2 Hook
// ---------------------------------------------------------------------------

DEFINE_HOOK(&dMeter2Draw_c::draw, FairyMeter2DrawHook);

// Icon files per [fairy color][button]. Color order = FairyColor_e (Navi, Tatl, Tael)
static constexpr int k_iconColors = 3;
static constexpr int k_iconButtons = 2;

static const char* const k_iconFiles[k_iconColors][k_iconButtons] = {
    { "textures/Navi_Lkey_00.bti", "textures/Navi_Rkey_00.bti" },  // Navi
    { "textures/Talt_Lkey_00.bti", "textures/Talt_Rkey_00.bti" },  // Tatl
    { "textures/Teal_Lkey_00.bti", "textures/Teal_Rkey_00.bti" },  // Tael
};

static ResourceBuffer s_iconBuf[k_iconColors][k_iconButtons] = {
    { RESOURCE_BUFFER_INIT, RESOURCE_BUFFER_INIT },
    { RESOURCE_BUFFER_INIT, RESOURCE_BUFFER_INIT },
    { RESOURCE_BUFFER_INIT, RESOURCE_BUFFER_INIT },
};
static J2DPicture* s_iconPic[k_iconColors][k_iconButtons] = {};
static bool        s_iconTried[k_iconColors][k_iconButtons] = {};

static JGeometry::TBox2<f32> s_cachedJujiBounds(0.0f, 0.0f, 0.0f, 0.0f);
static bool                  s_hasCachedBounds = false;

// Returns the icon for that fairy/button, or nullptr if the file is missing
static J2DPicture* get_icon_slot(int c, int b) {
    if (s_iconPic[c][b] != nullptr) {
        return s_iconPic[c][b];
    }
    if (s_iconTried[c][b]) {
        return nullptr;
    }
    s_iconTried[c][b] = true;

    if (svc_resource->load(mod_ctx, k_iconFiles[c][b], &s_iconBuf[c][b]) != MOD_OK
        || s_iconBuf[c][b].data == nullptr) {
        return nullptr;
    }

    JKRHeap* rootHeap = JKRHeap::getRootHeap();
    JKRHeap* oldHeap = (rootHeap != nullptr) ? mDoExt_setCurrentHeap(rootHeap) : nullptr;
    s_iconPic[c][b] = JKR_NEW J2DPicture(reinterpret_cast<const ResTIMG*>(s_iconBuf[c][b].data));
    if (oldHeap != nullptr) {
        mDoExt_setCurrentHeap(oldHeap);
    }
    return s_iconPic[c][b];
}

static J2DPicture* get_fairy_icon(u8 colorId, bool isRight) {
    const int c = (colorId < k_iconColors) ? colorId : 0;
    const int b = isRight ? 1 : 0;

    J2DPicture* pic = get_icon_slot(c, b);
    if (pic == nullptr && b == 1) {
        pic = get_icon_slot(c, 0); // Fall back to the Lkey icon if the Rkey file is missing
    }
    return pic;
}

void maFairyHud_shutdown() {
    for (int c = 0; c < k_iconColors; c++) {
        for (int b = 0; b < k_iconButtons; b++) {
            if (s_iconPic[c][b] != nullptr) {
                JKR_DELETE(s_iconPic[c][b]);
                s_iconPic[c][b] = nullptr;
            }
            svc_resource->free(mod_ctx, &s_iconBuf[c][b]);
            s_iconTried[c][b] = false;
        }
    }
    s_hasCachedBounds = false;
}

// Precomputed 24-segment unit circle (cos, sin) for fallback disc rendering
static void fairy_draw_disc(f32 cx, f32 cy, f32 r, JUtility::TColor c) {
    constexpr int k_segments = 24;
    static const struct { f32 cos; f32 sin; } k_unitCircle[k_segments + 1] = {
        {  1.00000000f,  0.00000000f },
        {  0.96592583f,  0.25881905f },
        {  0.86602540f,  0.50000000f },
        {  0.70710678f,  0.70710678f },
        {  0.50000000f,  0.86602540f },
        {  0.25881905f,  0.96592583f },
        {  0.00000000f,  1.00000000f },
        { -0.25881905f,  0.96592583f },
        { -0.50000000f,  0.86602540f },
        { -0.70710678f,  0.70710678f },
        { -0.86602540f,  0.50000000f },
        { -0.96592583f,  0.25881905f },
        { -1.00000000f,  0.00000000f },
        { -0.96592583f, -0.25881905f },
        { -0.86602540f, -0.50000000f },
        { -0.70710678f, -0.70710678f },
        { -0.50000000f, -0.86602540f },
        { -0.25881905f, -0.96592583f },
        {  0.00000000f, -1.00000000f },
        {  0.25881905f, -0.96592583f },
        {  0.50000000f, -0.86602540f },
        {  0.70710678f, -0.70710678f },
        {  0.86602540f, -0.50000000f },
        {  0.96592583f, -0.25881905f },
        {  1.00000000f,  0.00000000f },
    };

    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_SET);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_CLR_RGBA, GX_F32, 0);
    GXBegin(GX_TRIANGLEFAN, GX_VTXFMT0, k_segments + 2);
    GXPosition3f32(cx, cy, 0.0f);
    GXColor1u32(c);
    for (int s = 0; s <= k_segments; s++) {
        GXPosition3f32(cx + k_unitCircle[s].cos * r, cy + k_unitCircle[s].sin * r, 0.0f);
        GXColor1u32(c);
    }
    GXEnd();
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_CLR_RGBA, GX_RGBA4, 0);
}

// ---- Icon placement (adjust these to taste) ----
static constexpr f32 k_hudOffsetX = 0.0f; //0.0f;
static constexpr f32 k_hudOffsetY = 0.0f; // 25.0f = centered on juji, 0.0f = top of juji
static constexpr f32 k_hudRadius = 9.0f;

static void on_meter2_draw_fairy_post(ModContext*, void* args, void*, void*) {
    if (args == nullptr) {
        return;
    }

    dMeter2Draw_c* draw = mods::arg<dMeter2Draw_c*>(args, 0);
    if (draw == nullptr || draw->getMainScreenPtr() == nullptr) {
        return;
    }

    J2DScreen* screen = draw->getMainScreenPtr();
    J2DPane* juji = screen->search(MULTI_CHAR('juji_n'));

    if (juji != nullptr) {
        const JGeometry::TBox2<f32>& b = juji->getGlbBounds();
        if (b.getWidth() > 0.0f && b.getHeight() > 0.0f) {
            s_cachedJujiBounds = b;
            s_hasCachedBounds = true;
        }
    }

    static u32 s_lastTick = 0;
    static int s_staleCount = 0;
    constexpr int k_pauseStaleFrames = 8;   // más alto = tarda más en ocultarse
    if (maFairyCompanion_c::sExecTick != s_lastTick) {
        s_lastTick = maFairyCompanion_c::sExecTick;
        s_staleCount = 0;
    }
    else if (s_staleCount < 1000) {
        s_staleCount++;
    }
    const bool paused = (s_staleCount > k_pauseStaleFrames);

    if (paused || !maFairyCompanion_c::sAbilityReady || !s_hasCachedBounds) {
        return;
    }

    //if (!maFairyCompanion_c::sAbilityReady || !s_hasCachedBounds) {
    //    return;
    //}

    J2DGrafContext* ctx = dComIfGp_getCurrentGrafPort();
    if (ctx != nullptr) {
        ctx->setup2D();
    }

    const JGeometry::TBox2<f32>& bounds = (juji != nullptr && juji->getGlbBounds().getWidth() > 0.0f)
        ? juji->getGlbBounds()
        : s_cachedJujiBounds;

    const bool isRight = (maFairyCompanion_c::sActionButton == FAIRY_BUTTON_RIGHT);
    const f32  baseX = isRight ? (bounds.i.x + bounds.getWidth() + 6.0f) : (bounds.i.x - 6.0f);
    const f32  cx = baseX + (isRight ? k_hudOffsetX : -k_hudOffsetX);
    const f32  cy = bounds.i.y + bounds.getHeight() * 0.5f + k_hudOffsetY;

    const FairyTint_s& tint = getFairyTint(maFairyCompanion_c::sSelectedColor);

    J2DPicture* pic = get_fairy_icon(maFairyCompanion_c::sSelectedColor, isRight);
    if (pic != nullptr) {
        constexpr f32 k_iconScale = 0.60f;          // <-- 0.75 = 75%, 1.5 = 150%
        constexpr f32 w = 64.0f * k_iconScale;
        constexpr f32 h = 32.0f * k_iconScale;
        pic->setAlpha(255);
        const f32 iconX = isRight ? (bounds.i.x + bounds.getWidth() - 64.0f)   // Rkey: empieza a la derecha de la cruceta //const f32 iconX = isRight ? (bounds.i.x + bounds.getWidth() - 64.0f)
            : (bounds.i.x - 6.0f + 10.0f);              // Lkey: termina a la izquierda de la cruceta //: (bounds.i.x - 6.0f  + 10.0f);
        pic->draw(iconX, cy - h * 0.5f, w, h, false, false, false);
    }
    else {
        fairy_draw_disc(cx, cy, k_hudRadius,
            JUtility::TColor(tint.bodyR, tint.bodyG, tint.bodyB, 255));
    }
}

void maFairyHud_init() {
    mods::hook::add_post<FairyMeter2DrawHook>(on_meter2_draw_fairy_post);
}

