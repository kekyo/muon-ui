import { increment } from 'muon:counter';
import { increment as vendorIncrement } from 'muon:vendor_counter';
let value = 0;
document.querySelector<HTMLButtonElement>('#increment')!.onclick = async () => {
  value = await increment(value);
  value = await vendorIncrement(value);
  document.querySelector('#result')!.textContent = String(value);
};
