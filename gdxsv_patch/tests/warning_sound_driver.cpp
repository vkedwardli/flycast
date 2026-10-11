// Exercise the pinned game driver with Flycast's real ARM interpreter.
// Hardware access is stubbed: this checks controller dispatch and total level,
// with the common bank's velocity curve and other gains at their maximum.
#include <fstream>
#include <cmath>
#include "StandaloneCheck.h"
#include "hw/arm7/arm7.cpp"
namespace aica { std::array<u8,2*1024*1024> aica_ram; void timeStep() {} }
namespace aica::arm {
bool aica_interr, e68k_out; u32 aica_reg_L, e68k_reg_L, e68k_reg_M;
template<typename T> T readReg(u32) { abort(); }
template<typename T> void writeReg(u32,T) { abort(); }
static void step() {
    reg[15].I = armNextPC + 8;
    int clockTicks = 0;
    #include "hw/arm7/arm-new.h"
}
}
static void load(const char* path, unsigned addr) {
    std::ifstream f(path,std::ios::binary); CHECK(f);
    f.read((char*)aica::aica_ram.data()+addr, aica::aica_ram.size()-addr);
}
static u32 call(u32 pc) {
    using namespace aica::arm;
    const u32 stop=0x1f0000; arm_Reg[13].I=0x1e0000;
    *(u32*)&aica::aica_ram[0x1e0000]=stop;
    arm_Reg[R15_ARM_NEXT].I=pc;
    unsigned ops=0;
    while(arm_Reg[R15_ARM_NEXT].I!=stop) { ++ops; CHECK(ops<10000); step(); }
    return arm_Reg[0].I;
}
static int tl(int vol) {
    using namespace aica::arm;
    reset();
    // Real common bank's velocity curve, with the remaining gains at stock max.
    constexpr u32 work=0x10000, voice=0x11000, chan=0xc800, bank=0x20000;
    aica::aica_ram[work+5]=127;
    *(u32*)&aica::aica_ram[voice]=bank;
    aica::aica_ram[voice+6]=127;
    aica::aica_ram[chan]=0;
    // Route port 0 / channel 12 to driver channel slot 0.
    aica::aica_ram[0xe800+11*4]=0;
    for(int i=1;i<4;i++) aica::aica_ram[0xe800+11*4+i]=255;
    arm_Reg[12].I=work; arm_Reg[11].I=65535;
    arm_Reg[0].I=0xbb270000 | (vol<<8);
    call(0x45c0);
    CHECK(aica::aica_ram[chan+10]==vol);
    aica::aica_ram[chan+16]=127;
    arm_Reg[12].I=work; arm_Reg[10].I=voice; arm_Reg[11].I=chan;
    arm_Reg[0].I=0; arm_Reg[1].I=127; arm_Reg[2].I=127;
    return call(0x4270);
}
static int adjusted;
static void parameter(int port, int channel, int command, int value) {
    CHECK(port==0 && channel==12 && command==0x27b0);
    adjusted=value;
}
// WARNING_HOOK
int main(int argc, char** argv) {
    CHECK(argc==3);
    load(argv[1],0);
    load(argv[2],0x20000);
    for(int v: {57,66,76,85,88,95,101,114,127}) {
        int orig=tl(v), old=tl(v/2);
        gdx_warning_sound_parameter(0,12,0x27b0,v);
        int fixed=tl(adjusted);
        printf("stock %d: TL %d; divided value %d TL %d (%.3f amplitude); offset value %d TL %d (%.3f amplitude)\n",v,orig,v/2,old,pow(2.,-(old-orig)/16.),adjusted,fixed,pow(2.,-(fixed-orig)/16.));
        CHECK(fixed-orig>=15 && fixed-orig<=17);
    }
}
