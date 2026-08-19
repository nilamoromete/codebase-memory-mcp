import { describe, expect, it } from "vitest";

import { formatUserLabel } from "../src/formatter";

describe("formatUserLabel", () => {
  it("trims surrounding whitespace", () => {
    expect(formatUserLabel(" Ada ")).toBe("Ada");
  });
});
