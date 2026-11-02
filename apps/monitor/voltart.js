// voltart.js — the Voltage view's drawings as plain boxes on a 280×156 stage: a small single-board computer or a
// desktop motherboard, ours and with no maker's marks. Each box is [x, y, w, h, color, radius]. `power` boxes take the
// color of the power state: the way from the power input to the processor, and the power light.
const METAL = '#b9c0c8', DARK = '#23272c', CHIP = '#2d3238', GOLD = '#d9b45b', BLACK = '#15171a';

const row = (n, x, y, w, h, gap, color, r = 1) => Array.from({ length: n }, (_, i) => [x + i * (w + gap), y, w, h, color, r]);

export const BOARD_ART = {
  pi: {
    boxes: [
      [40, 16, 200, 124, '#1f7a4d', 8],                         // the board
      ...[[46, 22], [226, 22], [46, 126], [226, 126]].map(([x, y]) => [x, y, 8, 8, GOLD, 4]),
      [60, 19, 128, 11, BLACK, 2],                              // the pin header
      ...row(20, 63, 22, 3, 5, 3, GOLD),
      [96, 58, 42, 42, CHIP, 4],                                // processor
      [103, 65, 28, 28, '#3b4148', 3],
      [148, 62, 28, 34, DARK, 3],                               // memory
      [62, 62, 20, 20, CHIP, 3],                                // power chip
      [186, 34, 54, 34, METAL, 3],                              // network
      [186, 72, 54, 26, METAL, 3],                              // USB
      [186, 102, 54, 26, METAL, 3],
      [192, 77, 42, 6, DARK, 1], [192, 87, 42, 6, DARK, 1], [192, 107, 42, 6, DARK, 1], [192, 117, 42, 6, DARK, 1],
      [98, 128, 22, 14, METAL, 3], [130, 128, 22, 14, METAL, 3],// video
      [50, 128, 30, 15, METAL, 6],                              // power input
    ],
    power: [[62, 122, 6, 8, null, 0], [62, 82, 6, 44, null, 0], [62, 72, 8, 8, null, 0], [82, 74, 16, 6, null, 0], [216, 132, 7, 7, null, 4]],
  },
  mother: {
    boxes: [
      [46, 8, 188, 140, '#2c3138', 6],                          // the board
      [52, 14, 44, 34, METAL, 2],                               // the back panel's ports
      [106, 18, 60, 54, DARK, 3],                               // processor socket
      [116, 28, 40, 34, METAL, 3],
      ...row(4, 176, 16, 7, 50, 5, '#4a5560', 2),               // memory
      [222, 90, 8, 44, BLACK, 1],                               // main power connector
      ...Array.from({ length: 6 }, (_, i) => [223, 94 + i * 7, 6, 3, GOLD, 0]),
      [56, 84, 130, 9, BLACK, 2],                               // expansion slots
      [56, 106, 130, 9, BLACK, 2],
      [56, 128, 90, 9, BLACK, 2],
      [160, 122, 34, 22, CHIP, 3],                              // chipset
      [58, 60, 36, 12, CHIP, 2],                                // power stage
    ],
    power: [[200, 108, 22, 6, null, 0], [200, 78, 6, 34, null, 0], [146, 74, 58, 6, null, 0], [146, 72, 6, 6, null, 0], [208, 134, 7, 7, null, 4]],
  },
};
