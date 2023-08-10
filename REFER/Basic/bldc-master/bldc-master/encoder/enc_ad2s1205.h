/*
	Copyright 2016 - 2022 Benjamin Vedder	benjamin@vedder.se
	Copyright 2022 Marcos Chaparro	mchaparro@powerdesigns.ca
	Copyright 2022 Jakub Tomczak

	This file is part of the VESC firmware.

	The VESC firmware is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    The VESC firmware is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef ENC_AD2S1205_H_
#define ENC_AD2S1205_H_

#include "datatypes.h"
#include "encoder/encoder_datatype.h"

// Functions
bool enc_ad2s1205_init(AD2S1205_config_t *AD2S1205_config);
void enc_ad2s1205_deinit(AD2S1205_config_t *cfg);
void enc_ad2s1205_routine(AD2S1205_config_t *cfg);
void enc_ad2s1205_reset_errors(AD2S1205_config_t *cfg);

// Macros
#define AD2S1205_LAST_ANGLE(cfg)		((cfg)->state.last_enc_angle)

#endif /* ENC_AD2S1205_H_ */
