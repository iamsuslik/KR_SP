# 1. Переходим в папку сборки
cd build

# 2. Пересобираем проект (обновит и СУБД, и тесты, если код менялся)
cmake --build .

# 3. Запускаем тесты для проверки
./unit_tests
./my_db

---

### отправить
git checkout main
git pull origin main
git merge seven
git push origin main

### залить
git checkout main
git pull origin main
git checkout six
git merge main