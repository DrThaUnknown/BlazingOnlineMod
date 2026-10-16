import { defineConfig } from "vitest/config";

export default defineConfig({
  test: { include: ["host/test/**/*.test.ts"] },
});
