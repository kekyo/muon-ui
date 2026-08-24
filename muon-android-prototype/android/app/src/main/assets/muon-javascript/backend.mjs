/* Packaged ES module used to verify the Android QuickJS sidecar. */

let counter = 0;

export const answer = 42;

export const increment = () => {
  counter += 1;
  return counter;
};

export const echo = async (value) => value;

export const invokeCallback = async (value, callback) => await callback(value);

export const exhaustMemory = () => {
  const blocks = [];
  while (true) {
    blocks.push(new Uint8Array(1024 * 1024));
  }
};

export const spin = () => {
  while (true) {
    // The runtime interrupt handler must terminate runaway application code.
  }
};

globalThis.__muonBackendModule = Object.freeze({
  answer,
  increment,
  echo,
  invokeCallback,
  exhaustMemory,
  spin,
});
