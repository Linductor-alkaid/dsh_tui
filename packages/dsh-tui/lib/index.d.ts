export const name: string;
export const inject: string[];
export function apply(ctx: unknown, config: { resumeSessionId?: string; binaryPath?: string }): () => Promise<void>;
