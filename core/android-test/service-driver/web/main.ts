// A real document owner; navigation must release its native service references.
const status = document.getElementById('status')!;
document.getElementById('release')!.onclick = () => {
  location.href = './empty.html';
};
try {
  const api = Reflect.get(window, 'muon');
  const alpha = await api.services.acquire('alpha.v1-test');
  const beta = await api.services.acquire('beta');
  const watch = await api.services.watchState(
    'alpha.v1-test',
    (state: unknown) => {
      document.getElementById('watch')!.textContent =
        'page-watch ' + JSON.stringify(state);
    }
  );
  document.getElementById('autostart-off')!.onclick = async () => {
    try {
      await api.services.setAutostart('alpha.v1-test', false);
      status.textContent = 'page-autostart-off';
    } catch (error) {
      status.textContent = 'page-error ' + String(error);
    }
  };
  document.getElementById('close-watch')!.onclick = async () => {
    try {
      await watch.close();
      status.textContent = 'page-watch-closed';
    } catch (error) {
      status.textContent = 'page-error ' + String(error);
    }
  };
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
