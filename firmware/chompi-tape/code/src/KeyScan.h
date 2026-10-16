#pragma once

#include <stdint.h>
#include <algorithm>
#include "daisy_seed.h"
#include "util/hal_map.h"

namespace chompi
{
    // A chain of CD4021s read by SPI: Start starts the transfer, Take reads it a block later. Debounced like
    // libDaisy's ShiftRegister4021.
    class KeyScan
    {
      public:
        static constexpr int kMax = 40;

        struct Config
        {
            SPI_TypeDef *spi;
            uint8_t af;
            uint8_t mbr; // the bit clock: the SPI's kernel clock / 2^(mbr + 1)
            daisy::Pin clk, data, latch;
            int chips, dbc_size;
        };

        void Init(const Config &c)
        {
            spi_ = c.spi;
            bytes_ = c.chips;
            dbc_size_ = c.dbc_size;
            for(int i = 0; i < kMax; i++)
            {
                raw_[i] = risen_[i] = false;
                dbc_[i] = 0;
            }
            updated_ = started_ = false;
            last_ = 0;

            if(spi_ == SPI1)
                __HAL_RCC_SPI1_CLK_ENABLE();
            else
                __HAL_RCC_SPI6_CLK_ENABLE();
            for(const dsy_gpio_pin p : {dsy_gpio_pin(c.clk), dsy_gpio_pin(c.data)})
            {
                dsy_hal_map_gpio_clk_enable(p.port);
                GPIO_InitTypeDef g = {dsy_hal_map_get_pin(&p), GPIO_MODE_AF_PP, GPIO_NOPULL, GPIO_SPEED_FREQ_LOW, c.af};
                HAL_GPIO_Init(dsy_hal_map_get_port(&p), &g);
            }
            latch_.Init(c.latch, daisy::GPIO::Mode::OUTPUT);
            latch_.Write(true); // high: the registers load, low: they shift

            spi_->CR1 = SPI_CR1_SSI;
            // the clock idles high: a bit is read on the falling edge, before the rising one shifts the next in
            spi_->CFG1 = (uint32_t(c.mbr) << SPI_CFG1_MBR_Pos) | (7u << SPI_CFG1_DSIZE_Pos);
            spi_->CFG2 = SPI_CFG2_MASTER | SPI_CFG2_CPOL | SPI_CFG2_SSM | SPI_CFG2_AFCNTR | (2u << SPI_CFG2_COMM_Pos);
        }

        void Start()
        {
            if(started_)
                return;
            latch_.Write(false);
            spi_->CR2 = bytes_;
            spi_->CR1 = SPI_CR1_SSI | SPI_CR1_SPE;
            spi_->CR1 = SPI_CR1_SSI | SPI_CR1_SPE | SPI_CR1_CSTART;
            started_ = true;
        }

        void Take()
        {
            updated_ = false;
            if(!started_ || !(spi_->SR & SPI_SR_EOT))
                return;
            uint8_t in[kMax / 8];
            for(int k = 0; k < bytes_; k++)
                in[k] = *reinterpret_cast<volatile uint8_t *>(&spi_->RXDR);
            spi_->IFCR = SPI_IFCR_EOTC | SPI_IFCR_TXTFC;
            spi_->CR1 = SPI_CR1_SSI;
            latch_.Write(true);
            started_ = false;

            const int n = bytes_ * 8; // the first bit in is the last input
            for(int i = 0; i < n; i++)
                raw_[n - 1 - i] = in[i / 8] >> (7 - i % 8) & 1;

            const uint32_t now = daisy::System::GetNow();
            if(now == last_)
                return;
            last_ = now;
            updated_ = true;
            for(int i = 0; i < n; i++)
            {
                int d = dbc_[i] + raw_[i] * 2;
                if(d > 0)
                    d--;
                dbc_[i] = uint16_t(std::min(d, dbc_size_ * 2));
            }
        }

        bool State(int i) const { return dbc_[i] < dbc_size_; }
        bool RawState(int i) const { return raw_[i]; }

        bool RisingEdge(int i)
        {
            if(risen_[i] || !State(i) || !updated_)
                return false;
            risen_[i] = true;
            return true;
        }

        bool FallingEdge(int i)
        {
            if(!risen_[i] || State(i) || !updated_)
                return false;
            risen_[i] = false;
            return true;
        }

      private:
        SPI_TypeDef *spi_;
        daisy::GPIO latch_;
        int bytes_, dbc_size_;
        bool raw_[kMax], risen_[kMax];
        uint16_t dbc_[kMax];
        bool updated_, started_;
        uint32_t last_;
    };
}
