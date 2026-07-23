module.exports = {
  cjsEcho: async (value) => value,
  cjsThis(value) {
    return `${this.cjsValue}:${value}`;
  },
  cjsValue: 'commonjs',
};
