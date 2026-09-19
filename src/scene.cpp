#include "scene.h"

void Scene::add(const MeshInstance& object)
{
    objects.push_back(object);
}

std::vector<MeshInstance>& Scene::getObjects()
{
    return objects;
}

const std::vector<MeshInstance>& Scene::getObjects() const
{
    return objects;
}