// A real document owner; navigation must release its native service references.
const status = document.getElementById('status')!;
document.getElementById('release')!.onclick = () => {
  location.href = './empty.html';
};
try {
  const api = Reflect.get(window, 'muon');
  const alpha = await api.services.acquire('alpha.v1-test');
  const beta = await api.services.acquire('beta');
  const betaState = await api.services.getState('beta');
  status.textContent =
    'page-ready ' +
    JSON.stringify({
      uiPid: await api.environments.getProcessId(),
      alphaGeneration: alpha.generation,
      betaGeneration: beta.generation,
      betaClientReferences: betaState.clientReferences,
    });
} catch (error) {
  status.textContent = 'page-error ' + String(error);
}
export {};
