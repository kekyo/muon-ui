// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

export const marker = 'expected module namespace';

export const then = (resolve) => {
  resolve({
    hijacked: true,
  });
};
