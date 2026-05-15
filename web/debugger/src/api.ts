import type {
  ComponentRef,
  DiscoveredServer,
  EntityDetail,
  EntitySummary,
  GraphQLResponse,
  JobDetail,
  JobSummary,
  RegisteredTag,
  ServerSnapshot,
  SingletonInstance
} from "./types";

const DEFAULT_TIMEOUT_MS = 700;
export type Fetcher = (input: RequestInfo | URL, init?: RequestInit) => Promise<Response>;

const browserFetch: Fetcher = (input, init) => globalThis.fetch(input, init);

export function candidatePorts(basePort: number): number[] {
  return Array.from({ length: 16 }, (_, index) => basePort + index);
}

export function serverUrl(port: number): string {
  return `http://127.0.0.1:${port}/graphql`;
}

export class GraphQLClient {
  constructor(private readonly url: string, private readonly fetchImpl: Fetcher = browserFetch) {}

  async request<T>(query: string, variables?: Record<string, unknown>, timeoutMs = DEFAULT_TIMEOUT_MS): Promise<T> {
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), timeoutMs);
    try {
      const response = await this.fetchImpl(this.url, {
        method: "POST",
        headers: { "Content-Type": "text/plain" },
        body: JSON.stringify({ query, variables }),
        signal: controller.signal
      });
      if (!response.ok) {
        throw new Error(`HTTP ${response.status}`);
      }
      const payload = (await response.json()) as GraphQLResponse<T>;
      if (payload.errors?.length) {
        throw new Error(payload.errors.map((error) => error.message).join("; "));
      }
      if (!payload.data) {
        throw new Error("GraphQL response did not include data");
      }
      return payload.data;
    } finally {
      clearTimeout(timeout);
    }
  }
}

export const ENTITIES_QUERY = `
  query Entities {
    entities { id index version kind displayName }
  }
`;

export const SINGLETONS_QUERY = `
  query Singletons {
    singletons {
      entity { id index version kind displayName }
      component { component name tag singleton dirty debugValue fields { name type value } }
    }
  }
`;

export const JOBS_QUERY = `
  query Jobs {
    jobs { id name order structural singleThread maxThreads minEntitiesPerThread reads { component name singleton } writes { component name singleton } }
  }
`;

export const REGISTERED_TAGS_QUERY = `
  query RegisteredTags {
    registeredTags { component name }
  }
`;

export const SERVER_NAME_QUERY = `
  query ServerName {
    serverName
  }
`;

export const ENTITY_QUERY = `
  query EntityDetail($id: ID!) {
    entity(id: $id) {
      id index version kind displayName
      components { component name tag singleton dirty debugValue fields { name type value } }
      matchingJobs { id name order structural singleThread maxThreads minEntitiesPerThread reads { component name singleton } writes { component name singleton } }
    }
  }
`;

export const JOB_QUERY = `
  query JobDetail($id: ID!) {
    job(id: $id) {
      id name order structural singleThread maxThreads minEntitiesPerThread reads { component name singleton } writes { component name singleton }
      matchingEntities { id index version kind displayName }
    }
  }
`;

export const REMOVE_COMPONENT_MUTATION = `
  mutation RemoveComponent($entity: ID!, $component: ID!) {
    removeComponent(entity: $entity, component: $component) { ok }
  }
`;

export const SET_COMPONENT_MUTATION = `
  mutation SetComponent($entity: ID!, $component: ID!, $value: JSON) {
    setComponent(entity: $entity, component: $component, value: $value) {
      component name tag singleton dirty debugValue fields { name type value }
    }
  }
`;

export async function fetchSnapshot(client: GraphQLClient): Promise<ServerSnapshot> {
  const [name, entities, singletons, jobs, registeredTags] = await Promise.all([
    client.request<{ serverName?: string }>(SERVER_NAME_QUERY).catch(() => ({ serverName: "" })),
    client.request<{ entities: EntitySummary[] }>(ENTITIES_QUERY),
    client.request<{ singletons: SingletonInstance[] }>(SINGLETONS_QUERY).catch(() => ({ singletons: [] })),
    client.request<{ jobs: JobSummary[] }>(JOBS_QUERY),
    client.request<{ registeredTags: RegisteredTag[] }>(REGISTERED_TAGS_QUERY)
  ]);
  return {
    name: name.serverName?.trim() ?? "",
    entities: entities.entities,
    singletons: singletons.singletons,
    jobs: jobs.jobs,
    registeredTags: registeredTags.registeredTags
  };
}

export async function fetchEntity(client: GraphQLClient, id: string): Promise<EntityDetail | null> {
  const data = await client.request<{ entity: EntityDetail | null }>(ENTITY_QUERY, { id });
  return data.entity;
}

export async function fetchJob(client: GraphQLClient, id: string): Promise<JobDetail | null> {
  const data = await client.request<{ job: JobDetail | null }>(JOB_QUERY, { id });
  return data.job;
}

export async function removeComponent(client: GraphQLClient, entity: string, component: string): Promise<boolean> {
  const data = await client.request<{ removeComponent: { ok: boolean } }>(REMOVE_COMPONENT_MUTATION, {
    entity,
    component
  });
  return data.removeComponent.ok;
}

export async function setComponent(
  client: GraphQLClient,
  entity: string,
  component: string,
  value: Record<string, unknown>
) {
  const data = await client.request<{ setComponent: unknown }>(SET_COMPONENT_MUTATION, { entity, component, value });
  return data.setComponent;
}

export async function addTag(client: GraphQLClient, entity: string, component: string) {
  return setComponent(client, entity, component, {});
}

export async function probeServer(port: number, fetchImpl: Fetcher = browserFetch): Promise<DiscoveredServer> {
  const url = serverUrl(port);
  const client = new GraphQLClient(url, fetchImpl);
  const start = performance.now();
  try {
    const snapshot = await fetchSnapshot(client);
    return { port, url, ok: true, latencyMs: Math.round(performance.now() - start), snapshot };
  } catch (error) {
    return {
      port,
      url,
      ok: false,
      latencyMs: Math.round(performance.now() - start),
      error: error instanceof Error ? error.message : String(error)
    };
  }
}

export async function discoverServers(basePort: number, fetchImpl: Fetcher = browserFetch): Promise<DiscoveredServer[]> {
  return Promise.all(candidatePorts(basePort).map((port) => probeServer(port, fetchImpl)));
}

export function entityLabel(entity: EntitySummary): string {
  return entity.displayName || `${entity.kind} #${entity.index} v${entity.version}`;
}

export function componentName(componentId: string | ComponentRef, components: { component: string; name: string }[]): string {
  if (typeof componentId !== "string") {
    return componentRefName(componentId);
  }
  return components.find((component) => component.component === componentId)?.name || componentId;
}

export function jobTitle(job: JobSummary): string {
  return job.name || `Job #${job.id} order ${job.order}`;
}

export function componentRefId(component: ComponentRef | string): string {
  return typeof component === "string" ? component : component.component;
}

export function componentRefName(component: ComponentRef | string): string {
  return typeof component === "string" ? component : component.name || component.component;
}

export function componentRefIsSingleton(component: ComponentRef | string): boolean {
  return typeof component === "string" ? false : Boolean(component.singleton);
}
