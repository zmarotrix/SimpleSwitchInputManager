// XInput mode: an Xbox Wireless Controller (Bluetooth HID) built from WinUHid's
// preset. Windows' own xinputhid driver turns it into a real XInput device.

#include "output.h"
#include "protocol.h"

#include "WinUHidXOne.h"

#include <algorithm>

namespace {

class XInputOutput : public OutputBackend {
public:
    ~XInputOutput() override { Destroy(); }

    bool Create(int slot) override
    {
        wchar_t instance[32];
        swprintf_s(instance, L"SSIMXbox%d", slot);
        WINUHID_PRESET_DEVICE_INFO info = {};
        info.InstanceID = instance;
        // The preset's own IDs are replaced by ours, so keep the "&IG_00" ID that
        // makes Windows' xinputhid driver claim the pad as an XInput controller.
        info.HardwareIDs = L"HID\\VID_045E&PID_02FF&IG_00\0";
        m_pad = WinUHidXOneCreate(&info, &XInputOutput::OnFF, this);
        if (!m_pad) Log("XInput pad: create failed (%lu)", GetLastError());
        return m_pad != nullptr;
    }

    void Destroy() override
    {
        if (m_pad) {
            WinUHidXOneDestroy(m_pad);
            m_pad = nullptr;
        }
    }

    bool RumbleStopsWithApp() const override { return true; }

    bool Submit(const PadInput& in) override
    {
        if (!m_pad) return false;
        WINUHID_XONE_INPUT_REPORT r;
        WinUHidXOneInitializeInputReport(&r);
        const uint32_t b = in.buttons;
        r.LeftStickX = AxisTo16(in.lx);
        r.LeftStickY = AxisTo16(-in.ly);  // HID: +Y is down
        r.RightStickX = AxisTo16(in.rx);
        r.RightStickY = AxisTo16(-in.ry);
        r.LeftTrigger = (USHORT)std::clamp((int)(in.lt * 1023.f + 0.5f), 0, 1023);
        r.RightTrigger = (USHORT)std::clamp((int)(in.rt * 1023.f + 0.5f), 0, 1023);
        // Positional: Xbox A is the bottom face button, which is Nintendo B.
        r.ButtonA = !!(b & kBtnB);
        r.ButtonB = !!(b & kBtnA);
        r.ButtonX = !!(b & kBtnY);
        r.ButtonY = !!(b & kBtnX);
        r.ButtonLB = !!(b & kBtnL);
        r.ButtonRB = !!(b & kBtnR);
        r.ButtonBack = !!(b & kBtnMinus);
        r.ButtonMenu = !!(b & kBtnPlus);
        r.ButtonLS = !!(b & kBtnLStick);
        r.ButtonRS = !!(b & kBtnRStick);
        r.ButtonHome = !!(b & kBtnHome);
        int hx = (b & kBtnRight ? 1 : 0) - (b & kBtnLeft ? 1 : 0);
        int hy = (b & kBtnDown ? 1 : 0) - (b & kBtnUp ? 1 : 0);
        WinUHidXOneSetHatState(&r, hx, hy);
        r.BatteryLevel = (UCHAR)std::clamp(in.batteryPercent * 255 / 100, 0, 255);
        return WinUHidXOneReportInput(m_pad, &r) != FALSE;
    }

private:
    // Motor values are 0-100. Big (left) motor -> low band, small (right) -> high
    // band; impulse trigger motors add high band to their own side.
    static VOID OnFF(PVOID ctx, UCHAR left, UCHAR right, UCHAR leftTrigger, UCHAR rightTrigger)
    {
        auto* self = (XInputOutput*)ctx;
        RumbleCommand c;
        c.lowL = c.lowR = left / 100.f;
        c.highL = std::min(1.f, right / 100.f + leftTrigger / 100.f);
        c.highR = std::min(1.f, right / 100.f + rightTrigger / 100.f);
        self->Rumble(c);
    }

    PWINUHID_XONE_GAMEPAD m_pad = nullptr;
};

}  // namespace

std::unique_ptr<OutputBackend> CreateXInputOutput()
{
    return std::make_unique<XInputOutput>();
}
