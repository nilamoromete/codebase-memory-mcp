import { formatUserLabel } from "./formatter";

export function profileHeading(name: string): string {
  return `Profile: ${formatUserLabel(name)}`;
}
