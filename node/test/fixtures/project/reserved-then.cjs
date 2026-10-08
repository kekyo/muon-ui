// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

const exportsObject = {};

Object.defineProperty(exportsObject, 'then', {
  enumerable: true,
  value: async (value) => value,
});

module.exports = exportsObject;
