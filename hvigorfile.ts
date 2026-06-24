import { appTasks } from '@ohos/hvigor-ohos-plugin';

const { ensureDevEcoEnvironment } = require('./scripts/deveco_hvigor_env.js');

ensureDevEcoEnvironment(__dirname);

export default {
  system: appTasks, /* Built-in plugin of Hvigor. It cannot be modified. */
  plugins: []       /* Custom plugin to extend the functionality of Hvigor. */
}
