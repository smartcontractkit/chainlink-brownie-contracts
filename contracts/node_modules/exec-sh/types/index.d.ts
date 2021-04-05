declare module 'exec-sh' {
  import { SpawnOptionsWithoutStdio, ChildProcessWithoutNullStreams } from 'child_process';

  export type execShCallback = (error: Error | null, stdout: string, stderr: string) => void;

  declare function execSh (command: string | string[], options?: SpawnOptionsWithoutStdio | boolean, callback?: execShCallback): ChildProcessWithoutNullStreams;
  declare function execSh (command: string | string[], callback?: execShCallback): ChildProcessWithoutNullStreams;
  declare namespace execSh {
    export const promise: (command: string | string[], options?: SpawnOptionsWithoutStdio) => Promise<{ stdout: string, stderr: string }>;
  }

  export default execSh;
}