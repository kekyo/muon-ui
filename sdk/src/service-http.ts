import type { ServiceCancellationSignal } from './service-connection.ts';
import type { ServiceIoBridge } from './service-io.ts';

/** An outbound HTTP request authorized by this service's configured origins. */
export interface ServiceHttpRequest {
  /** Absolute HTTP or HTTPS URL. */ readonly url: string;
  /** HTTP method, default GET. */ readonly method?: string;
  /** Additional request headers. */ readonly headers?: Readonly<
    Record<string, string>
  >;
  /** UTF-8 text or binary request body. */ readonly body?: string | Uint8Array;
  /** Response representation; defaults to bytes. */ readonly responseType?:
    'text' | 'bytes';
  /** Total time including redirects, from 1 to 120000 ms; defaults to 30000. */ readonly timeoutMs?: number;
  /** Operation cancellation; defaults to the generation lifetime. */ readonly signal?: ServiceCancellationSignal;
}

/** A completed HTTP response, including 4xx and 5xx statuses. @typeParam Body - Requested body representation. */
export interface ServiceHttpResponse<Body extends string | Uint8Array> {
  /** HTTP status code. */ readonly status: number;
  /** Lowercase header names. Repeated fields are joined with a comma. */ readonly headers: Readonly<
    Record<string, string>
  >;
  /** Response body owned by the caller. */ readonly body: Body;
}

/** Generation-bound outbound HTTP and HTTPS client. */
export interface ServiceHttpClient {
  /** Sends one request. @param request URL, headers, body, representation and cancellation. @returns Completed HTTP response. */
  request(
    request: ServiceHttpRequest & { responseType: 'text' }
  ): Promise<ServiceHttpResponse<string>>;
  /** @param request Binary request options. @returns Status, headers and owned bytes. */
  request(
    request: ServiceHttpRequest & { responseType?: 'bytes' }
  ): Promise<ServiceHttpResponse<Uint8Array>>;
  /** @param request Request with a dynamically selected representation. @returns Status, headers and the selected body. */
  request(
    request: ServiceHttpRequest
  ): Promise<ServiceHttpResponse<string | Uint8Array>>;
}

/** @internal Builds the HTTP facade without requiring DOM or Node globals. */
export const createServiceHttp = (
  io: ServiceIoBridge,
  lifetime: ServiceCancellationSignal
): ServiceHttpClient => {
  function request(
    options: ServiceHttpRequest & { responseType: 'text' }
  ): Promise<ServiceHttpResponse<string>>;
  function request(
    options: ServiceHttpRequest & { responseType?: 'bytes' }
  ): Promise<ServiceHttpResponse<Uint8Array>>;
  function request(
    options: ServiceHttpRequest
  ): Promise<ServiceHttpResponse<string | Uint8Array>>;
  async function request(
    options: ServiceHttpRequest
  ): Promise<ServiceHttpResponse<string | Uint8Array>> {
    const {
      url,
      method = 'GET',
      headers = {},
      body,
      responseType = 'bytes',
      timeoutMs = 30000,
      signal = lifetime,
    } = options;
    if (
      body !== undefined &&
      typeof body !== 'string' &&
      !(body instanceof Uint8Array)
    )
      throw new TypeError('Invalid HTTP body');
    const reply = await io(
      'http.request',
      {
        url,
        method,
        headers,
        responseType,
        timeoutMs,
        ...(typeof body === 'string' ? { text: body } : {}),
      },
      body instanceof Uint8Array ? body.slice() : new Uint8Array(),
      signal
    );
    const meta = reply.metadata as {
      status: number;
      headers: Record<string, string>;
      text?: string;
    };
    return {
      status: meta.status,
      headers: Object.freeze(meta.headers),
      body: responseType === 'text' ? meta.text! : reply.body,
    };
  }
  return Object.freeze({ request });
};
