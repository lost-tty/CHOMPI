// Drawing units are 0.1 mm at the panel's drawn width

// firmware LED colours
export const LED = {
  blue: '#19B4E6', green: '#2EB82E', yellow: '#E6C200', orange: '#F08C3C', teal: '#1FC9B8',
  purple: '#8C3CF0', magenta: '#D23CD2', red: '#E03030', pink: '#F06E9E',
  deep: '#3046E8', lime: '#A8D81E', white: '#F2F2F2',
};

export const SANS = 'Helvetica Neue, Helvetica, Arial, sans-serif';
export const MONO = 'Rubik, Helvetica Neue, Arial, sans-serif'; // the body font: labels read as words, not code
export const CW = 905; // drawing width
export const FS = 32;
export const SM = 26;

// panel geometry in mm, from the enclosure DXF / PCB, y up from the panel bottom
export const PW = 325;
export const PH = 105;
export const KNOBS = [72.0, 105.5, 139.055, 172.573, 303.284];
export const KNOB_LEDS = [41.856, 72.018, 105.536, 139.055, 172.573, 303.283];
export const SMALL_LEDS = { wheel_l: 202.745, wheel_r: 222.856, play: 242.967, loop: 263.078 };
export const WHEEL = [212.795, 71.36, 17.75];
export const WHITE = Array.from({ length: 15 }, (_, i) => 25.5 + 20.111 * i);
export const BLACK = { 16: 35.555, 17: 55.666, 18: 95.889, 19: 116.0, 20: 136.111, 21: 176.333,
  22: 196.444, 23: 236.667, 24: 256.778, 25: 276.889 };
export const KEYS = { shift: [45.6, 68.8, 22], play: [246.8, 68.8, 15], loop: [266.9, 68.8, 15],
  mic: [BLACK[18], 39.5, 15], line: [BLACK[19], 39.5, 15], copy: [BLACK[24], 39.5, 15] };
export const POS = { ...KEYS, wheel: WHEEL, ...Object.fromEntries(KNOBS.map((k, i) => [`knob${i}`, [k, 71.36]])) };

// one decimal is plenty
export const r = (n) => Math.round(n * 10) / 10;
