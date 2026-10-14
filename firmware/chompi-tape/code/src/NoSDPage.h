#pragma GCC push_options
#pragma GCC optimize ("Os") // cold code, built for size so all of it fits in SRAM
#include "hardware.h"
#include "DSPEngine.h"
#include "Leds.h"

namespace chompi
{
    class NoSDPage : public daisy::UiPage
    {
    public:
        void Init() {}

        void Draw(const daisy::UiCanvasDescriptor &canvasDescriptor) override
        {
            uint32_t now = System::GetNow();
            
            if(now - last_blink_time > 400)
            {
                last_blink_time = now;
                blink = !blink;

                for(int i = 0; i < LedFrame::kPth; i++)
                {
                    leds.ui.Pth(i, blink * 1.f, 0.f, 0.f);
                }

                for(int i = 0; i < LedFrame::kSmt; i++)
                {
                    leds.ui.Smt(i, blink * 1.f, 0.f, 0.f);
                }
            }

            // ========   send the data   =========
            leds.Show();
        }

        bool OnEncoderTurned(uint16_t encoderID,
                             int16_t turns,
                             uint16_t stepsPerRevolution) override
        {
            return true; // do nothing
        }

        bool OnButton(uint16_t buttonID,
                uint8_t numberOfPresses,
                bool isRetriggering) override
        {
            return true; // do nothing
        }

        void OnFocusGained() override {}

    private:
        uint32_t last_blink_time;
        bool blink;
    };
} // namespace chompi

#pragma GCC pop_options
