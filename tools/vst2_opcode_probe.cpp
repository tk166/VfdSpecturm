// 探针：把镜像 VST2 SDK 里各 opcode 的实际数值打出来，与 VST 2.4 规范值对照。
// 若不一致，就是"插件与 EAPO 说不同方言"的根源。
#include <cstdio>

#include "aeffectx.h"

int main() {
    std::printf("%-38s %6s  %6s\n", "opcode", "here", "canon");
    struct Row { const char* name; int here; int canon; };
    const Row rows[] = {
        {"audioMasterAutomate",                 audioMasterAutomate, 0},
        {"audioMasterVersion",                  audioMasterVersion, 1},
        {"audioMasterCurrentId",                audioMasterCurrentId, 2},
        {"audioMasterIdle",                     audioMasterIdle, 3},
        {"audioMasterGetTime",                  audioMasterGetTime, 7},
        {"audioMasterProcessEvents",            audioMasterProcessEvents, 8},
        {"audioMasterSizeWindow",               audioMasterSizeWindow, 15},
        {"audioMasterGetSampleRate",            audioMasterGetSampleRate, 16},
        {"audioMasterGetBlockSize",             audioMasterGetBlockSize, 17},
        {"audioMasterGetCurrentProcessLevel",   audioMasterGetCurrentProcessLevel, 23},
        {"audioMasterGetVendorString",          audioMasterGetVendorString, 32},
        {"audioMasterGetProductString",         audioMasterGetProductString, 33},
        {"audioMasterGetVendorVersion",         audioMasterGetVendorVersion, 34},
        {"audioMasterCanDo",                    audioMasterCanDo, 37},
        {"audioMasterGetLanguage",              audioMasterGetLanguage, 38},
        {"audioMasterGetDirectory",             audioMasterGetDirectory, 41},
        {"audioMasterUpdateDisplay",            audioMasterUpdateDisplay, 42},
        {"audioMasterBeginEdit",                audioMasterBeginEdit, 43},
        {"audioMasterEndEdit",                  audioMasterEndEdit, 44},
        {"effOpen",                             effOpen, 0},
        {"effClose",                            effClose, 1},
        {"effSetProgram",                       effSetProgram, 2},
        {"effGetProgram",                       effGetProgram, 3},
        {"effGetProgramName",                   effGetProgramName, 5},
        {"effGetParamName",                     effGetParamName, 8},
        {"effSetSampleRate",                    effSetSampleRate, 10},
        {"effSetBlockSize",                     effSetBlockSize, 11},
        {"effMainsChanged",                     effMainsChanged, 12},
        {"effEditGetRect",                      effEditGetRect, 13},
        {"effEditOpen",                         effEditOpen, 14},
        {"effEditClose",                        effEditClose, 15},
        {"effGetChunk",                         effGetChunk, 23},
        {"effSetChunk",                         effSetChunk, 24},
        {"effGetEffectName",                    effGetEffectName, 45},
        {"effGetVendorString",                  effGetVendorString, 47},
        {"effGetProductString",                 effGetProductString, 48},
        {"effGetVstVersion",                    effGetVstVersion, 58},
        {"effStartProcess",                     effStartProcess, 71},
        {"effStopProcess",                      effStopProcess, 72},
    };
    int bad = 0;
    for (const auto& r : rows) {
        const bool ok = (r.here == r.canon);
        if (!ok) ++bad;
        std::printf("%-38s %6d  %6d  %s\n", r.name, r.here, r.canon, ok ? "" : "<== MISMATCH");
    }
    std::printf("\n%d mismatches\n", bad);
    return bad == 0 ? 0 : 1;
}
