export type EntityKind = "Invalid" | "User" | "Component" | "Job" | "System";

export interface EntitySummary {
  id: string;
  index: number;
  version: number;
  kind: EntityKind;
  displayName?: string;
}

export interface ComponentInstance {
  component: string;
  name: string;
  tag: boolean;
  singleton: boolean;
  dirty: boolean;
  debugValue: string;
  fields: ComponentFieldValue[];
}

export type ComponentFieldType = "bool" | "i32" | "u32" | "i64" | "u64" | "f32" | "f64" | "string";

export interface ComponentFieldValue {
  name: string;
  type: ComponentFieldType;
  value: string | boolean;
}

export interface JobSummary {
  id: string;
  name: string;
  order: number;
  structural: boolean;
  singleThread: boolean;
  maxThreads: number;
  minEntitiesPerThread: number;
  reads: Array<ComponentRef | string>;
  writes: Array<ComponentRef | string>;
}

export interface RegisteredTag {
  component: string;
  name: string;
}

export interface ComponentRef {
  component: string;
  name: string;
  singleton?: boolean;
}

export interface SingletonInstance {
  entity: EntitySummary;
  component: ComponentInstance;
}

export interface EntityDetail extends EntitySummary {
  components: ComponentInstance[];
  matchingJobs: JobSummary[];
}

export interface JobDetail extends JobSummary {
  matchingEntities: EntitySummary[];
}

export interface ServerSnapshot {
  name: string;
  entities: EntitySummary[];
  singletons: SingletonInstance[];
  jobs: JobSummary[];
  registeredTags: RegisteredTag[];
}

export interface DiscoveredServer {
  port: number;
  url: string;
  ok: boolean;
  latencyMs: number;
  error?: string;
  snapshot?: ServerSnapshot;
}

export interface GraphQLError {
  message: string;
}

export interface GraphQLResponse<T> {
  data?: T;
  errors?: GraphQLError[];
}
