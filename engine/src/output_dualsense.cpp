// DualSense mode: a PS5 controller built from WinUHid's preset. Steam, SDL games
// and emulators read its gyro, accelerometer and rumble natively.

#include "output.h"
#include "protocol.h"

#include "WinUHidPS5.h"

#include <algorithm>

namespace {

constexpr float kGravity = 9.80665f;
constexpr float kDegToRad = 3.14159265f / 180.f;

class DualSenseOutput : public OutputBackend {
public:
    ~DualSenseOutput() override { Destroy(); }

    bool Create(int slot) override
    {
        wchar_t instance[32];
        swprintf_s(instance, L"SSIMDualSense%d", slot);
        WINUHID_PRESET_DEVICE_INFO basic = {};
        basic.InstanceID = instance;
        WINUHID_PS5_GAMEPAD_INFO info = {};
        info.BasicInfo = &basic;
        // Stable, locally administered MAC per slot so games keep their bindings.
        const UCHAR mac[6] = {0x02, 0x53, 0x53, 0x49, 0x4D, (UCHAR)(slot + 1)};
        memcpy(info.MacAddress, mac, 6);
        m_pad = WinUHidPS5Create(&info, &DualSenseOutput::OnRumble, nullptr, nullptr, nullptr, this);
        if (!m_pad) Log("DualSense pad: create failed (%lu)", GetLastError());
        return m_pad != nullptr;
    }

    void Destroy() override
    {
        if (m_pad) {
            WinUHidPS5Destroy(m_pad);
            m_pad = nullptr;
        }
    }

    bool Submit(const PadInput& in) override
    {
        if (!m_pad) return false;
        WINUHID_PS5_INPUT_REPORT r;
        WinUHidPS5InitializeInputReport(&r);
        const uint32_t b = in.buttons;
        r.LeftStickX = AxisTo8(in.lx);
        r.LeftStickY = AxisTo8(-in.ly);  // DualSense: +Y is down
        r.RightStickX = AxisTo8(in.rx);
        r.RightStickY = AxisTo8(-in.ry);
        r.LeftTrigger = (UCHAR)std::clamp((int)(in.lt * 255.f + 0.5f), 0, 255);
        r.RightTrigger = (UCHAR)std::clamp((int)(in.rt * 255.f + 0.5f), 0, 255);
        // Positional: Cross is the bottom face button, which is Nintendo B.
        r.ButtonCross = !!(b & kBtnB);
        r.ButtonCircle = !!(b & kBtnA);
        r.ButtonSquare = !!(b & kBtnY);
        r.ButtonTriangle = !!(b & kBtnX);
        r.ButtonL1 = !!(b & kBtnL);
        r.ButtonR1 = !!(b & kBtnR);
        r.ButtonL2 = !!(b & kBtnZL);
        r.ButtonR2 = !!(b & kBtnZR);
        r.ButtonShare = !!(b & kBtnMinus);
        r.ButtonOptions = !!(b & kBtnPlus);
        r.ButtonL3 = !!(b & kBtnLStick);
        r.ButtonR3 = !!(b & kBtnRStick);
        r.ButtonHome = !!(b & kBtnHome);
        r.ButtonTouchpad = !!(b & kBtnCapture);
        r.ButtonMute = !!(b & kBtnC);
        int hx = (b & kBtnRight ? 1 : 0) - (b & kBtnLeft ? 1 : 0);
        int hy = (b & kBtnDown ? 1 : 0) - (b & kBtnUp ? 1 : 0);
        WinUHidPS5SetHatState(&r, hx, hy);
        if (in.hasMotion) {
            // PadInput motion uses the DSU (Cemuhook) convention. DualSense uses SDL's:
            // gravity reads +Y when lying flat, so accel flips on every axis and
            // gyro flips yaw and roll (matches SDL's own Switch 2 driver).
            WinUHidPS5SetAccelState(&r, -in.accel[0] * kGravity, -in.accel[1] * kGravity, -in.accel[2] * kGravity);
            WinUHidPS5SetGyroState(&r, in.gyro[0] * kDegToRad, -in.gyro[1] * kDegToRad, -in.gyro[2] * kDegToRad);
        }
        WinUHidPS5SetBatteryState(&r, in.wired, (UCHAR)std::clamp(in.batteryPercent, 0, 100));
        return WinUHidPS5ReportInput(m_pad, &r) != FALSE;
    }

private:
    // Left motor is the heavy (low-frequency) one, right the light one; both 0-255.
    static VOID OnRumble(PVOID ctx, UCHAR left, UCHAR right)
    {
        auto* self = (DualSenseOutput*)ctx;
        RumbleCommand c;
        c.lowL = c.lowR = left / 255.f;
        c.highL = c.highR = right / 255.f;
        self->Rumble(c);
    }

    PWINUHID_PS5_GAMEPAD m_pad = nullptr;
};

}  // namespace

std::unique_ptr<OutputBackend> CreateDualSenseOutput()
{
    return std::make_unique<DualSenseOutput>();
}
