'use strict';

const callable = function () {
  return this === undefined;
};

callable.label = 'callable-commonjs';
callable.method = function (value) {
  return `${this.label}:${value}`;
};

module.exports = callable;
