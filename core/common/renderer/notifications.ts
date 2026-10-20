// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

/** Values preserved when notification data is serialized and restored. */
export type MuonNotificationData =
  | null
  | boolean
  | number
  | string
  | readonly MuonNotificationData[]
  | { readonly [key: string]: MuonNotificationData };

/** A named notification action, independently of support for starting an exited app. */
export interface MuonNotificationAction {
  /** Nonempty ID of up to 128 UTF-16 code units, unique within this notification. */
  readonly id: string;
  /** Nonempty user-visible label of up to 128 UTF-16 code units. */
  readonly title: string;
}

/** Application-owned local notification. */
export interface MuonNotificationOptions {
  /** Stable app-scoped ID, up to 128 UTF-16 code units; reposting updates it. */
  readonly id: string;
  /** Nonempty display title, up to 1024 UTF-16 code units. */
  readonly title: string;
  /** Body text, up to 4096 UTF-16 code units; may be empty. */
  readonly body: string;
  /** Purpose key, up to 128 code units; omission uses notifications.defaultCategory. */
  readonly category?: string;
  /** JSON data for notification interaction; maximum encoded size is 16384 UTF-8 bytes and depth is 64. */
  readonly data?: MuonNotificationData;
  /** Up to three named action buttons; requires the actions capability. */
  readonly actions?: readonly MuonNotificationAction[];
}

/** One persisted operation on a posted notification revision. */
export interface MuonNotificationEvent {
  /** Stable identifier for deduplication and explicit acknowledgement. */
  readonly eventId: string;
  /** Application notification ID supplied to post(). */
  readonly id: string;
  /** Default notification activation or a named action button. */
  readonly type: 'activate' | 'action';
  /** Named action ID, or null for default activation. */
  readonly actionId: string | null;
  /** Data from the operated revision; null when post() omitted data. */
  readonly data: MuonNotificationData;
}

/** Receives notification events serially; returning does not acknowledge them. */
export type MuonNotificationListener = (
  event: MuonNotificationEvent
) => void | Promise<void>;

/** A page-owned subscription that does not own persisted events. */
export interface MuonNotificationWatcher {
  /**
   * Releases this subscription and cancels its pending native wait.
   * @returns Completion of release; repeated calls share the same operation.
   * @remarks May be awaited inside the listener. Already running listeners are
   * not awaited, and later listeners are prevented. Pending events remain intact.
   */
  readonly close: () => Promise<void>;
}

/** Support for a feature, independently of current notification permission. */
export interface MuonNotificationSupport {
  /** Support established from this implementation and its environment. */
  readonly state: 'supported' | 'unsupported' | 'unknown';
  /** Diagnostic reason when applicable; null when no explanation is needed. */
  readonly reason: string | null;
}

/** Features available in the current implementation and OS environment. */
export interface MuonNotificationCapabilities {
  /** Delivery of notification operations to the running application. */
  readonly interaction: MuonNotificationSupport;
  /** Starting an exited application and delivering its notification operation. */
  readonly launch: MuonNotificationSupport;
  /** Keeping posted notifications after normal application process exit. */
  readonly persistence: MuonNotificationSupport;
  /** Additional named action buttons. */
  readonly actions: MuonNotificationSupport;
  /** Reading application-wide posting permission. */
  readonly appStatus: MuonNotificationSupport;
  /** Reading OS controls for an individual purpose category. */
  readonly categoryStatus: MuonNotificationSupport;
}

/** Current posting permission observation; not evidence of display or reading. */
export interface MuonNotificationPostingState {
  /** Whether posting is enabled, disabled, or cannot be determined. */
  readonly state: 'enabled' | 'disabled' | 'unknown';
  /** Diagnostic reason, or null when no explanation is needed. */
  readonly reason: string | null;
}

/** Application state and, only when requested, category-specific state. */
export interface MuonNotificationStatus {
  /** Current application-wide permission observation. */
  readonly app: MuonNotificationPostingState;
  /** Category observation, independently of app state; null for an app-only query. */
  readonly category:
    | (MuonNotificationPostingState & {
        /** Requested category ID. */
        readonly id: string;
      })
    | null;
}

/** Shared native local notification operations, present only in enabled builds. */
export interface MuonNotificationsApi {
  /**
   * Posts or replaces one notification using its app-scoped stable ID.
   * @param options - Content, category and JSON data for this notification.
   * @returns Completion of the OS API submission, not delivery or viewing confirmation.
   * @remarks Never requests permission implicitly. Rejects known permission or
   * channel disablement, unregistered Android categories and unsupported options.
   * On event-capable backends, unoperated revisions and unacknowledged events
   * share 256 reserved slots. A full store rejects new posts. Updating an ID
   * invalidates its unoperated older revision and reuses that slot; previously
   * received events remain intact. Unoperated revisions expire after 30 days.
   * Removal by the user or OS does not release its reserved slot; use
   * cancel(), replace that ID, or let the revision expire.
   * OS submission and local persistence are not one atomic transaction.
   * Linux updates submit a distinct OS notification before withdrawing the old
   * revision; both may briefly appear during replacement.
   * Android APK replacement may clear posted notifications; they are not
   * automatically reposted after an update.
   */
  readonly post: (options: MuonNotificationOptions) => Promise<void>;
  /**
   * Withdraws one notification without affecting other IDs.
   * @param id - Stable app-scoped notification ID.
   * @returns Completion; a missing notification is also successful.
   * @remarks Invalidates an unoperated revision without removing its received events.
   */
  readonly cancel: (id: string) => Promise<void>;
  /**
   * Reads fresh posting state, separately from feature support.
   * @param category - Optional purpose key; omission queries only the application.
   * @returns App-wide state and the explicitly requested category, if any.
   */
  readonly getStatus: (category?: string) => Promise<MuonNotificationStatus>;
  /**
   * Reads support for each notification feature in the current environment.
   * @returns Support and diagnostics; does not imply user permission.
   * @remarks Linux's direct notification backend cannot launch an exited app.
   * A service advertising persistence does not establish retention after process
   * exit, so that backend reports unknown unless the service lacks persistence.
   */
  readonly getCapabilities: () => Promise<MuonNotificationCapabilities>;
  /**
   * Reads persisted, unacknowledged operations without consuming them.
   * @returns Events retained across page and process destruction, oldest first.
   * @remarks Subscribe before reading, then deduplicate by eventId. Events expire
   * 30 days after receipt. Only the first operation on a revision creates an
   * event and withdraws its notification. Late operations on an updated,
   * cancelled or expired unoperated revision are ignored.
   * Android events survive APK updates retaining the application ID and data;
   * uninstalling or clearing application data removes the stored events.
   */
  readonly getPendingEvents: () => Promise<readonly MuonNotificationEvent[]>;
  /**
   * Subscribes to newly received operations in this page context.
   * @param listener - Serial event handler; errors are logged without acknowledging.
   * @returns A handle delivered before any listener invocation.
   * @remarks This does not replay existing events. Subscribe before calling
   * getPendingEvents(), and deduplicate by eventId. Other pages may receive the
   * same event; another page's acknowledgement can prevent later delivery.
   * Transport errors close the subscription after logging. Page destruction
   * releases the subscription without deleting application-owned events.
   */
  readonly watchEvents: (
    listener: MuonNotificationListener
  ) => Promise<MuonNotificationWatcher>;
  /**
   * Removes one event from the application's pending set after processing.
   * @param eventId - Stable ID received through getPendingEvents() or watchEvents().
   * @returns Durable completion; unknown or previously acknowledged IDs are no-ops.
   * @remarks Acknowledgement affects all pages and does not revoke callbacks
   * already dispatched. Up to 1024 acknowledged IDs are kept for 30 days;
   * unknown operation tokens are ignored even after that record expires.
   */
  readonly acknowledgeEvent: (eventId: string) => Promise<void>;
}

/**
 * Validates a notification identifier without changing the caller's value.
 * @param value - ID or category supplied at an API boundary.
 * @param label - Diagnostic field name.
 * @returns The unchanged string, or throws for an invalid identifier.
 */
export const notificationIdentifier = (
  value: unknown,
  label: string
): string => {
  if (
    typeof value !== 'string' ||
    value.trim() === '' ||
    value.includes('\0') ||
    value.length > 128
  )
    throw new TypeError(
      `${label} must be a nonempty string of at most 128 code units without NUL`
    );
  return value;
};

const validateData = (
  value: unknown,
  ancestors: Set<object>,
  depth: number
): void => {
  if (value === null || typeof value === 'string' || typeof value === 'boolean')
    return;
  if (typeof value === 'number' && Number.isFinite(value)) return;
  if (typeof value !== 'object' || value === null)
    throw new TypeError('Notification data must contain only JSON values');
  if (depth >= 64)
    throw new TypeError('Notification data exceeds 64 nested containers');
  if (ancestors.has(value))
    throw new TypeError('Notification data must not be circular');
  if (
    !Array.isArray(value) &&
    Object.getPrototypeOf(value) !== Object.prototype &&
    Object.getPrototypeOf(value) !== null
  )
    throw new TypeError('Notification data objects must be plain objects');
  if (Object.getOwnPropertySymbols(value).length > 0)
    throw new TypeError('Notification data must not contain symbol properties');
  ancestors.add(value);
  if (Array.isArray(value)) {
    for (let index = 0; index < value.length; index += 1)
      validateData(value[index], ancestors, depth + 1);
  } else {
    for (const item of Object.values(value))
      validateData(item, ancestors, depth + 1);
  }
  ancestors.delete(value);
};

const serializePost = (value: MuonNotificationOptions): string => {
  if (!value || typeof value !== 'object' || Array.isArray(value))
    throw new TypeError('Notification options must be an object');
  for (const key of Object.keys(value)) {
    if (!['id', 'title', 'body', 'category', 'data', 'actions'].includes(key))
      throw new TypeError(`Unsupported notification option: ${key}`);
  }
  notificationIdentifier(value.id, 'Notification id');
  if (
    typeof value.title !== 'string' ||
    value.title.trim() === '' ||
    value.title.length > 1024 ||
    value.title.includes('\0')
  )
    throw new TypeError(
      'Notification title must be nonempty, at most 1024 code units and without NUL'
    );
  if (
    typeof value.body !== 'string' ||
    value.body.length > 4096 ||
    value.body.includes('\0')
  )
    throw new TypeError(
      'Notification body must be a string of at most 4096 code units without NUL'
    );
  if (value.category !== undefined)
    notificationIdentifier(value.category, 'Notification category');
  if (value.data !== undefined) {
    validateData(value.data, new Set(), 0);
    if (new TextEncoder().encode(JSON.stringify(value.data)).length > 16384)
      throw new TypeError('Notification data exceeds 16384 UTF-8 bytes');
  }
  if (value.actions !== undefined) {
    if (!Array.isArray(value.actions) || value.actions.length > 3)
      throw new TypeError(
        'Notification actions must be an array of at most three actions'
      );
    const ids = new Set<string>();
    for (const action of value.actions) {
      if (!action || typeof action !== 'object' || Array.isArray(action))
        throw new TypeError('Notification action must be an object');
      for (const key of Object.keys(action)) {
        if (key !== 'id' && key !== 'title')
          throw new TypeError(`Unsupported notification action option: ${key}`);
      }
      const id = notificationIdentifier(action.id, 'Notification action id');
      notificationIdentifier(action.title, 'Notification action title');
      if (ids.has(id))
        throw new TypeError('Notification action IDs must be unique');
      ids.add(id);
    }
  }
  return JSON.stringify(value);
};

/**
 * Creates shared notification conversions over a native backend transport.
 * @param call - Authorized transport accepting a method name and wire arguments.
 * @returns Common notification API; the backend owns notifications across pages.
 */
export const createMuonNotificationsApi = (
  call: (
    name: string,
    arguments_: readonly unknown[],
    options?: { readonly signal?: AbortSignal }
  ) => Promise<unknown>
): MuonNotificationsApi => {
  const read = async <T>(
    name: string,
    args: readonly unknown[]
  ): Promise<T> => {
    const result = await call(name, args);
    if (typeof result !== 'string')
      throw new TypeError('Notification native JSON result is invalid');
    return JSON.parse(result) as T;
  };
  return Object.freeze({
    post: async (options: MuonNotificationOptions): Promise<void> => {
      await call('post', [serializePost(options)]);
    },
    cancel: async (id: string): Promise<void> => {
      await call('cancel', [notificationIdentifier(id, 'Notification id')]);
    },
    getStatus: async (category?: string) => {
      if (category !== undefined)
        notificationIdentifier(category, 'Notification category');
      return await read<MuonNotificationStatus>('getStatus', [
        JSON.stringify({ category }),
      ]);
    },
    getCapabilities: async () =>
      await read<MuonNotificationCapabilities>('getCapabilities', []),
    getPendingEvents: async () =>
      await read<readonly MuonNotificationEvent[]>('getPendingEvents', []),
    acknowledgeEvent: async (eventId: string): Promise<void> => {
      await call('acknowledgeEvent', [
        notificationIdentifier(eventId, 'Notification eventId'),
      ]);
    },
    watchEvents: async (
      listener: MuonNotificationListener
    ): Promise<MuonNotificationWatcher> => {
      if (typeof listener !== 'function')
        throw new TypeError('listener must be a function');
      const lease = await read<{ token: string; cursor: number }>(
        'watchEvents',
        [JSON.stringify({ operation: 'acquire' })]
      );
      const pending = new AbortController();
      let closed = false;
      let closing: Promise<void> | undefined;
      const close = async (): Promise<void> => {
        if (closing === undefined) {
          closed = true;
          pending.abort();
          closing = (async () => {
            try {
              await call('watchEvents', [
                JSON.stringify({ operation: 'release', token: lease.token }),
              ]);
            } catch {
              // Context destruction also releases its native subscription leases.
            }
          })();
        }
        await closing;
      };
      // The timer only defers startup until the handle is returned. Each next
      // request then waits for a native event without periodic polling.
      setTimeout(async () => {
        let cursor = lease.cursor;
        try {
          while (!closed) {
            const result = await call(
              'watchEvents',
              [
                JSON.stringify({
                  operation: 'next',
                  token: lease.token,
                  afterCursor: cursor,
                }),
              ],
              { signal: pending.signal }
            );
            if (typeof result !== 'string')
              throw new TypeError('Notification native JSON result is invalid');
            const batch = JSON.parse(result) as {
              events: MuonNotificationEvent[] | null;
              cursor: number;
            };
            if (batch.events === null || closed) break;
            cursor = batch.cursor;
            for (const event of batch.events) {
              if (closed) break;
              try {
                await listener(event);
              } catch (error) {
                console.error('Muon notification listener failed', error);
              }
            }
          }
        } catch (error) {
          if (!closed)
            console.error('Muon notification subscription failed', error);
        } finally {
          await close();
        }
      }, 0);
      return Object.freeze({ close });
    },
  });
};
