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
   */
  readonly post: (options: MuonNotificationOptions) => Promise<void>;
  /**
   * Withdraws one notification without affecting other IDs.
   * @param id - Stable app-scoped notification ID.
   * @returns Completion; a missing notification is also successful.
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
   */
  readonly getCapabilities: () => Promise<MuonNotificationCapabilities>;
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
    if (!['id', 'title', 'body', 'category', 'data'].includes(key))
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
  return JSON.stringify(value);
};

/**
 * Creates shared notification conversions over a native backend transport.
 * @param call - Authorized transport accepting a method name and wire arguments.
 * @returns Common notification API; the backend owns notifications across pages.
 */
export const createMuonNotificationsApi = (
  call: (name: string, arguments_: readonly unknown[]) => Promise<unknown>
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
  });
};
