export const name: string;
export const inject: string[];
export function apply(ctx: unknown, config: { resumeSessionId?: string; binaryPath?: string }): () => Promise<void>;

export interface WorkspaceStoreRecord {
  id: string;
  path: string;
  title: string;
  created: boolean;
}
export function prepareWorkspaceDirectory(input: string): string;
export function createWorkspaceRecordInStorage(canonicalPath: string, requestedTitle?: string): WorkspaceStoreRecord;
export function workspaceSnapshot(): {
  workspaces: Array<{ id: string; path: string; title: string; sessionIds: string[] }>;
  sessions: Array<{ id: string; title: string; cwd: string; workspaceId: string }>;
};
