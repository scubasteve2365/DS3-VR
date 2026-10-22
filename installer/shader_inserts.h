// Generated from Helifax's original shaders and the DS3 VR versions: pure insertions only.
// Each entry: shader file, the exact original line to insert after (must occur once), lines to insert.
#pragma once
#include "patch.h"

namespace setup {

inline const ShaderInsert kShaderInserts[] = {
    {L"ShaderFixesDM\\6c9e5a5657fff33d-vs.txt", "dcl_resource_buffer (float,float,float,float) t125",
     "dcl_resource_buffer (float,float,float,float) t60\n"},
    {L"ShaderFixesDM\\6c9e5a5657fff33d-vs.txt", "dp4 r1.y, v0.xyzw, cb0[1].xyzw",
     "// ds3vr: the lock-on marker. DS3 places it with its own camera; the mod (dinput8.dll) binds t60 for\n"
     "// that one draw: rows 0-2 turn a direction seen by the game's camera into the VR camera (head\n"
     "// rotation), row 3 = (tanX game, tanY game, 1/tanX VR, 1/tanY VR). Unbound (all other draws) = zeros.\n"
     "ld_indexable(buffer)(float,float,float,float) r5.xyzw, l(3, 0, 0, 0), t60.xyzw\n"
     "ne r4.w, r5.x, l(0.000000)\n"
     "if_nz r4.w\n"
     "  mul r2.x, r0.z, r5.x\n"
     "  mul r2.y, r1.y, r5.y\n"
     "  mov r2.z, l(1.000000)\n"
     "  ld_indexable(buffer)(float,float,float,float) r3.xyzw, l(0, 0, 0, 0), t60.xyzw\n"
     "  dp3 r4.x, r3.xyzx, r2.xyzx\n"
     "  ld_indexable(buffer)(float,float,float,float) r3.xyzw, l(1, 0, 0, 0), t60.xyzw\n"
     "  dp3 r4.y, r3.xyzx, r2.xyzx\n"
     "  ld_indexable(buffer)(float,float,float,float) r3.xyzw, l(2, 0, 0, 0), t60.xyzw\n"
     "  dp3 r4.z, r3.xyzx, r2.xyzx\n"
     "  max r4.z, r4.z, l(0.001000)\n"
     "  div r4.xy, r4.xyxx, r4.zzzz\n"
     "  mul r0.z, r4.x, r5.z\n"
     "  mul r1.y, r4.y, r5.w\n"
     "  mul r1.x, r0.x, r0.z\n"
     "endif\n"},
    {L"ShaderFixesDM\\f10647ba22ca9348-vs.txt", "dcl_resource_buffer (float,float,float,float) t125",
     "dcl_resource_buffer (float,float,float,float) t60\n"},
    {L"ShaderFixesDM\\f10647ba22ca9348-vs.txt", "dp4 r1.y, v1.xyzw, cb0[1].xyzw",
     "// ds3vr: the lock-on marker. DS3 places it with its own camera; the mod (dinput8.dll) binds t60 for\n"
     "// that one draw: rows 0-2 turn a direction seen by the game's camera into the VR camera (head\n"
     "// rotation), row 3 = (tanX game, tanY game, 1/tanX VR, 1/tanY VR). Unbound (all other draws) = zeros.\n"
     "ld_indexable(buffer)(float,float,float,float) r5.xyzw, l(3, 0, 0, 0), t60.xyzw\n"
     "ne r4.w, r5.x, l(0.000000)\n"
     "if_nz r4.w\n"
     "  mul r2.x, r0.y, r5.x\n"
     "  mul r2.y, r1.y, r5.y\n"
     "  mov r2.z, l(1.000000)\n"
     "  ld_indexable(buffer)(float,float,float,float) r3.xyzw, l(0, 0, 0, 0), t60.xyzw\n"
     "  dp3 r4.x, r3.xyzx, r2.xyzx\n"
     "  ld_indexable(buffer)(float,float,float,float) r3.xyzw, l(1, 0, 0, 0), t60.xyzw\n"
     "  dp3 r4.y, r3.xyzx, r2.xyzx\n"
     "  ld_indexable(buffer)(float,float,float,float) r3.xyzw, l(2, 0, 0, 0), t60.xyzw\n"
     "  dp3 r4.z, r3.xyzx, r2.xyzx\n"
     "  max r4.z, r4.z, l(0.001000)\n"
     "  div r4.xy, r4.xyxx, r4.zzzz\n"
     "  mul r0.y, r4.x, r5.z\n"
     "  mul r1.y, r4.y, r5.w\n"
     "  mul r1.x, r0.x, r0.y\n"
     "endif\n"},
    {L"ShaderFixesDM\\aa4d136778e07873-vs.txt", "dcl_resource_buffer (float,float,float,float) t125",
     "dcl_resource_buffer (float,float,float,float) t60\n"},
    {L"ShaderFixesDM\\aa4d136778e07873-vs.txt", "dp4 r2.y, v0.xyzw, cb0[r1.y + 0].xyzw",
     "// ds3vr: the lock-on marker. DS3 places it with its own camera; the mod (dinput8.dll) binds t60 for\n"
     "// that one draw: rows 0-2 turn a direction seen by the game's camera into the VR camera (head\n"
     "// rotation), row 3 = (tanX game, tanY game, 1/tanX VR, 1/tanY VR). Unbound (all other draws) = zeros.\n"
     "ld_indexable(buffer)(float,float,float,float) r6.xyzw, l(3, 0, 0, 0), t60.xyzw\n"
     "ne r5.w, r6.x, l(0.000000)\n"
     "if_nz r5.w\n"
     "  mul r3.x, r1.x, r6.x\n"
     "  mul r3.y, r2.y, r6.y\n"
     "  mov r3.z, l(1.000000)\n"
     "  ld_indexable(buffer)(float,float,float,float) r4.xyzw, l(0, 0, 0, 0), t60.xyzw\n"
     "  dp3 r5.x, r4.xyzx, r3.xyzx\n"
     "  ld_indexable(buffer)(float,float,float,float) r4.xyzw, l(1, 0, 0, 0), t60.xyzw\n"
     "  dp3 r5.y, r4.xyzx, r3.xyzx\n"
     "  ld_indexable(buffer)(float,float,float,float) r4.xyzw, l(2, 0, 0, 0), t60.xyzw\n"
     "  dp3 r5.z, r4.xyzx, r3.xyzx\n"
     "  max r5.z, r5.z, l(0.001000)\n"
     "  div r5.xy, r5.xyxx, r5.zzzz\n"
     "  mul r1.x, r5.x, r6.z\n"
     "  mul r2.y, r5.y, r6.w\n"
     "  mul r2.x, r0.x, r1.x\n"
     "endif\n"},
    {L"ShaderFixesDM\\fa3d4612573b6d52-vs.txt", "dcl_resource_buffer (float,float,float,float) t125",
     "dcl_resource_buffer (float,float,float,float) t60\n"},
    {L"ShaderFixesDM\\fa3d4612573b6d52-vs.txt", "dp4 r1.y, v1.xyzw, cb0[r0.w + 0].xyzw",
     "// ds3vr: the lock-on marker. DS3 places it with its own camera; the mod (dinput8.dll) binds t60 for\n"
     "// that one draw: rows 0-2 turn a direction seen by the game's camera into the VR camera (head\n"
     "// rotation), row 3 = (tanX game, tanY game, 1/tanX VR, 1/tanY VR). Unbound (all other draws) = zeros.\n"
     "ld_indexable(buffer)(float,float,float,float) r5.xyzw, l(3, 0, 0, 0), t60.xyzw\n"
     "ne r4.w, r5.x, l(0.000000)\n"
     "if_nz r4.w\n"
     "  mul r2.x, r0.z, r5.x\n"
     "  mul r2.y, r1.y, r5.y\n"
     "  mov r2.z, l(1.000000)\n"
     "  ld_indexable(buffer)(float,float,float,float) r3.xyzw, l(0, 0, 0, 0), t60.xyzw\n"
     "  dp3 r4.x, r3.xyzx, r2.xyzx\n"
     "  ld_indexable(buffer)(float,float,float,float) r3.xyzw, l(1, 0, 0, 0), t60.xyzw\n"
     "  dp3 r4.y, r3.xyzx, r2.xyzx\n"
     "  ld_indexable(buffer)(float,float,float,float) r3.xyzw, l(2, 0, 0, 0), t60.xyzw\n"
     "  dp3 r4.z, r3.xyzx, r2.xyzx\n"
     "  max r4.z, r4.z, l(0.001000)\n"
     "  div r4.xy, r4.xyxx, r4.zzzz\n"
     "  mul r0.z, r4.x, r5.z\n"
     "  mul r1.y, r4.y, r5.w\n"
     "  mul r1.x, r0.x, r0.z\n"
     "endif\n"},
    {L"ShaderFixesDM\\b68f3b72bc36908c-vs.txt", "dcl_constantbuffer CB12[1], immediateIndexed",
     "dcl_resource_texture2d (float,float,float,float) t100\n"},
    {L"ShaderFixesDM\\b68f3b72bc36908c-vs.txt", "or r0.x, r0.w, r0.x",
     "resinfo r1.xyzw, l(0), t100.xyzw\n"
     "div r1.z, r1.x, r1.y\n"
     "ge r1.w, r1.z, l(1.170000)\n"
     "lt r1.z, r1.z, l(1.200000)\n"
     "and r1.z, r1.z, r1.w\n"
     "ld r2.xyzw, l(3, 0, 0, 0), t120.xyzw\n"
     "mul r2.x, r2.x, l(0.400000)\n"
     "lt r1.w, r1.x, r2.x\n"
     "and r1.z, r1.z, r1.w\n"
     "or r0.x, r0.x, r1.z\n"},
};

} // namespace setup
