import { parseServiceInterface } from './service-interface.ts';
import type { ServiceInterface } from './service-interface.ts';
import type { ServiceClient, ServiceConnection } from './service-connection.ts';

/** A page handle exposing runtime metadata and already validated communication. */
export interface ServiceReferenceConnection {
  /** Metadata obtained from the acquired service generation. */
  readonly interface: unknown;
  /** Calls a method on that exact reference; no new reference is acquired. */
  readonly call: ServiceConnection['call'];
  /** Subscribes to an event on that exact reference. */
  readonly subscribe: ServiceConnection['subscribe'];
}

/**
 * Typed calls and events borrowing an acquired reference's lifetime.
 * @typeParam Definition - Required literal method and event signatures.
 */
export type BoundServiceClient<Definition extends ServiceInterface> = Pick<
  ServiceClient<Definition>,
  'call' | 'subscribe'
>;

/**
 * Checks a page handle against a shared interface and provides static call types.
 * @typeParam Definition - Required literal signatures, preserving argument types.
 * @param definition - Shared interface declared with defineServiceInterface.
 * @param reference - Acquired page handle; its owner remains responsible for release.
 * @returns Calls and subscriptions borrowing the handle, without another lifetime reference.
 * @throws TypeError when a required method, error set, or event signature differs.
 * @remarks Additional runtime methods/events are permitted. Releasing the original
 * handle cancels calls and subscriptions made through this client too.
 */
export const bindServiceClient = <const Definition extends ServiceInterface>(
  definition: Definition,
  reference: ServiceReferenceConnection
): BoundServiceClient<Definition> => {
  const expected = parseServiceInterface(definition);
  const actual = parseServiceInterface(reference.interface);
  for (const [name, method] of Object.entries(expected.methods)) {
    const found = actual.methods[name];
    if (
      !found ||
      JSON.stringify([
        method.parameters,
        method.returns,
        [...method.errors].sort(),
      ]) !==
        JSON.stringify([
          found.parameters,
          found.returns,
          [...found.errors].sort(),
        ])
    )
      throw new TypeError(
        'Service method does not match the required interface: ' + name
      );
  }
  for (const [name, type] of Object.entries(expected.events))
    if (JSON.stringify(type) !== JSON.stringify(actual.events[name]))
      throw new TypeError(
        'Service event does not match the required interface: ' + name
      );
  const connection: Pick<ServiceConnection, 'call' | 'subscribe'> = {
    call: async (method, args, options) =>
      await reference.call(method, args, options),
    subscribe: async (event, listener) =>
      await reference.subscribe(event, listener),
  };
  return Object.freeze(connection) as unknown as BoundServiceClient<Definition>;
};
