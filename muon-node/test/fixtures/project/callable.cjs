// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

'use strict';

const callable = function () {
  return this === undefined;
};

callable.label = 'callable-commonjs';
callable.method = function (value) {
  return `${this.label}:${value}`;
};

module.exports = callable;
