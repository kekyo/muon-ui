const exportsObject = {};

Object.defineProperty(exportsObject, 'then', {
  enumerable: true,
  value: async (value) => value,
});

module.exports = exportsObject;
