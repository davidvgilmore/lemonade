import type { ModelsData } from './modelData';
import { getCollectionComponents, isCollectionModel, isRouterCollection } from './collectionModels';

export async function prepareModelSelection(
  modelName: string,
  modelsData: ModelsData,
  actions: { selectRouter: (name: string) => Promise<void>; ensureReady: (name: string) => Promise<void> },
): Promise<void> {
  const info = modelsData[modelName];
  if (!info) throw new Error(`Missing model "${modelName}".`);
  if (isRouterCollection(info)) {
    await actions.selectRouter(modelName);
    return;
  }
  if (isCollectionModel(info)) {
    for (const component of getCollectionComponents(info)) {
      if (!modelsData[component]) throw new Error(`Missing component model "${component}" for ${modelName}.`);
      await actions.ensureReady(component);
    }
    return;
  }
  await actions.ensureReady(modelName);
}
