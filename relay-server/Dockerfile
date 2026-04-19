FROM node:18-alpine
WORKDIR /app
COPY package.json ./
RUN npm install --production
COPY index.js ./
EXPOSE 7000
CMD ["node", "index.js"]
