module.exports = {
  packageKind: 'bare-cjs',
  fromBareCjs: async (value) => `cjs:${value}`,
};
