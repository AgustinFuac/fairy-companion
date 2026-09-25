#include "ma_fairy_companion.hpp"
#include "d/d_com_inf_game.h"

#include <cmath>

// Mismo tamano de heap que usa daObjYOUSEI_c::create() para el mismo modelo +
// animacion (confirmado en d_a_obj_yousei.cpp). Como cargamos exactamente los
// mismos recursos, reutilizamos el mismo valor en vez de adivinar uno propio.
static constexpr u32 heap_size = 0x1100;

maFairyCompanion_c::~maFairyCompanion_c() {
    // "Always" es un archivo siempre residente (no se carga bajo demanda, por
    // eso daObjYOUSEI_c tampoco llama dComIfG_resDelete en su destructor). No
    // hay nada que liberar aqui aparte de lo que ya libera Delete().
}

cPhs_Step maFairyCompanion_c::create() {
    // El constructor de la clase no se llama automaticamente al crear el
    // actor: hay que invocarlo manualmente (mismo patron que ma_Mine_c /
    // maObj_Wrock_c).
    fopAcM_ct(this, maFairyCompanion_c);

    if (!fopAcM_entrySolidHeap(this, createHeapCallBack, heap_size)) {
        return cPhs_ERROR_e;
    }

    fopAcM_SetMtx(this, mpModelMorf->getModel()->getBaseTRMtx());
    fopAcM_SetMin(this, -50.0f, -50.0f, -50.0f);
    fopAcM_SetMax(this, 50.0f, 50.0f, 50.0f);

    mSound.init(&current.pos, &eyePos, 3, 1);

    mOrbitAngle = 0.0f;
    mColorId = FAIRY_COLOR_GREEN;
    mParticleId = 0;
    mParticleTimer = 0;
    mSparkleId = 0;
    mSparkleTimer = 0;

    // A PROPOSITO no hay nada aqui de ObjHit(), CareAction(),
    // dComIfGp_att_CatchRequest, ni contadores de autodestruccion: este actor
    // es enteramente nuestro. No interactuable "de fabrica" porque
    // simplemente no implementamos esa logica, no porque estemos bloqueando
    // estados de una clase ajena.

    Execute();
    return cPhs_COMPLEATE_e;
}

int maFairyCompanion_c::CreateHeap() {
    // Mismo patron de carga de recursos confirmado en
    // daObjYOUSEI_c::CreateHeap(): archivo "Always", indice 0x21 = modelo,
    // indice 0xF = animacion. No hace falta dComIfG_resLoad porque "Always"
    // ya esta residente en memoria.
    void* modelData = dComIfG_getObjectRes("Always", 0x21);
    if (modelData == NULL) {
        return 0;
    }

    mpModelMorf = JKR_NEW mDoExt_McaMorfSO((J3DModelData*)modelData, NULL, NULL,
        (J3DAnmTransform*)dComIfG_getObjectRes("Always", 0xF), 2, 0.4f, 0, -1, &mSound, 0x80000,
        0x11000084);

    return mpModelMorf != NULL;
}

int maFairyCompanion_c::createHeapCallBack(fopAc_ac_c* i_this) {
    return static_cast<maFairyCompanion_c*>(i_this)->CreateHeap();
}

int maFairyCompanion_c::Delete() {
    if (heap != NULL && mpModelMorf != NULL) {
        mpModelMorf->stopZelAnime();
    }
    this->~maFairyCompanion_c();
    return 1;
}

int maFairyCompanion_c::Execute() {
    daPy_py_c* player = daPy_getPlayerActorClass();

    if (player != NULL) {
        mOrbitAngle += 0.05f;
        if (mOrbitAngle > 6.2831853f) {
            mOrbitAngle -= 6.2831853f;
        }

        const f32 radius = 60.0f;
        const f32 heightOffset = 160.0f;  // Subido de 40 a 160: pendiente el
                                          // fix "de verdad" con un hueso de
                                          // Link (ver comentario abajo)

        current.pos.x = player->current.pos.x + radius * std::cos(mOrbitAngle);
        current.pos.y = player->current.pos.y + heightOffset;
        current.pos.z = player->current.pos.z + radius * std::sin(mOrbitAngle);

        // Orienta el hada hacia Link. fopAcM_searchPlayerAngleY() es la
        // misma funcion que ya usa daObjYOUSEI_c::Execute() (mAngleToPlayer),
        // asi que sabemos que da el angulo correcto en el sistema de
        // coordenadas del juego.
        shape_angle.y = fopAcM_searchPlayerAngleY(this);
    }

    // Intento de arreglo del color: sincronizamos al menos el room_no del
    // tevStr con nuestro propio room, igual que hacen ma_Mine_c/maObj_Wrock_c
    // cada frame (aunque ellos lo sacan de la colision con el suelo, y
    // nosotros no tenemos eso). Si el rosa persiste, el problema esta mas
    // probablemente en YukaCol, que todavia no podemos inicializar sin
    // d_kankyo_tev_str.h.
    tevStr.room_no = fopAcM_GetRoomNo(this);

    // Estela de particulas. El hada original combina varias particulas a la
    // vez (0x72F = brillo/estela suave, 0x730/0x731/0x732 = destellos en
    // forma de estrella segun el estado). Nosotros solo teniamos 0x72F;
    // anadimos 0x730 a otra cadencia para recuperar el destello de estrella
    // que se ve en el hada original.
    mParticleTimer++;
    if (mParticleTimer >= 8) {
        mParticleTimer = 0;
        mParticleId = dComIfGp_particle_set(
            mParticleId, 0x72F, &current.pos, &tevStr, &shape_angle, NULL, 0xFF, NULL, -1, NULL, NULL, NULL);
    }

    mSparkleTimer++;
    if (mSparkleTimer >= 8) {
        mSparkleTimer = 0;
        mSparkleId = dComIfGp_particle_set(
            mSparkleId, 0x731, &current.pos, &tevStr, &shape_angle, NULL, 0xFF, NULL, -1, NULL, NULL, NULL);
    }

    // Reproduce la animacion de vuelo, igual que hace daObjYOUSEI_c::Execute
    // cada frame.
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

int maFairyCompanion_c::Draw() {
    J3DModel* model = mpModelMorf->getModel();

    // Confirmado: estas dos llamadas son necesarias para que el modelo se
    // dibuje en absoluto (al quitarlas, desaparecio todo, no solo el color).
    // El color rosa sigue sin explicacion confirmada - ver nota abajo.
    g_env_light.settingTevStruct(0, &current.pos, &tevStr);
    g_env_light.setLightTevColorType_MAJI(model, &tevStr);

    mpModelMorf->entryDL();
    return 1;
}

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

s16 maFairyCompanion_c::sProcName = -1;
ActorHandle maFairyCompanion_c::sActorHandle = -1;
const ActorProfileDesc maFairyCompanion_c::sProfile = {
    .name = MFAIRYC_NAME,
    .priority_group = 7,  // mismo grupo que usan ma_Mine_c / maObj_Wrock_c
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
