// Build capability, independent of the host distribution ID.
export const standaloneDesktop = typeof __LORD_STANDALONE__ !== 'undefined' && __LORD_STANDALONE__;
